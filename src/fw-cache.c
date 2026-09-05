/* fw-cache.c — Pre-cache engine implementation
 *
 * Renders document pages asynchronously using a thread pool.
 * Pages are stored as cairo_surface_t* in a hash table keyed by page number.
 * Priority rendering: visible pages first, then forward, then backward.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-cache.h"
#include "fw-debug.h"

#include <math.h>
#include <stdlib.h>

/* A previous-zoom snapshot retained as a sharper-than-thumbnail
 * placeholder during continuous Ctrl+scroll zoom. Each slot carries
 * the (zoom, rotation, scale_factor) it was rendered at so
 * `fw_cache_get_texture` can pick the closest match to the current
 * zoom — Sioyek's `try_closest_rendered_page` pattern. */
typedef struct {
  cairo_surface_t *surface;
  GdkTexture      *texture;
  double           zoom;
  int              rotation;
  int              scale_factor;
  gsize            size_bytes;
} ZoomSlot;

/* Bound on retained previous-zoom snapshots per page. 3 slots cover
 * the typical "zoom in across 3-4 levels and back" workflow without
 * pinning surfaces indefinitely. Each slot's bytes count toward
 * total_cached_bytes so the cap still bounds RAM. */
#define MAX_PREV_ZOOM_SLOTS 3

typedef struct {
  cairo_surface_t *surface;        /* current-gen surface, NULL if not rendered */
  GdkTexture      *texture;        /* cached GPU texture wrapping surface */
  double           zoom;            /* zoom at which `surface` was rendered */
  int              rotation;        /* rotation at render time */
  int              scale_factor;    /* device scale factor at render time */
  gsize            size_bytes;     /* stride*height of `surface`, 0 if NULL */

  /* Multi-zoom retention: most recent demoted slot at index 0. Older
   * zooms slide right; oldest at MAX-1 evicts. fw_cache_get_texture
   * picks the closest by zoom (matching rotation+scale) when the
   * current-gen surface isn't ready. */
  ZoomSlot         prev_slots[MAX_PREV_ZOOM_SLOTS];
  int              prev_slot_count;
  gsize            prev_slots_bytes; /* sum of prev_slots[*].size_bytes */

  gboolean         rendering;      /* TRUE if a job is in the pool for this page */
  guint            render_gen;     /* render params generation when this surface was created */
  gint64           last_access_us; /* monotonic µs of last fw_cache_get_*: drives LRU eviction */
} CacheEntry;

/* Persistent low-resolution preview. Rendered once at open time (or first
 * access), never evicted. Serves as an always-available placeholder during
 * fast scroll and zoom transitions. */
typedef struct {
  cairo_surface_t *surface;
  GdkTexture      *texture;
  gboolean         rendering;
  gboolean         failed;       /* render attempted and failed — don't retry */
} ThumbEntry;

/* Target thumbnail width in pixels. At 150px wide × ~195px tall with 4 bytes
 * per pixel, each thumbnail is ~120KB. A 1000-page doc costs ~120MB — still
 * much less than the full cache tier, and gives instant visual feedback. */
#define THUMB_WIDTH 150.0

/* Tier 1: parsed page handles — lightweight backend objects (fz_page, ddjvu_page).
 * Wide window (~50 pages), negligible RAM cost, eliminates disk I/O on render. */
typedef struct {
  gpointer handle;   /* backend page object from fw_document_open_page() */
  guint    render_gen;
} ParsedEntry;

typedef struct {
  FwCache *cache;   /* weak — job checks if cache is still alive */
  int      page;
  double   zoom;
  int      rotation;
  int      scale_factor;  /* device scale captured at job creation (under lock) */
  guint    render_gen; /* render params generation at time of job creation */
  guint    cancel_gen; /* cancel generation at time of job creation */
  gint64   last_view_time; /* monotonic µs at job creation — sort key for the pool */
} RenderJob;

typedef enum {
  FW_RENDER_STATE_STATIC,
  FW_RENDER_STATE_CRUISING,
  FW_RENDER_STATE_SCRUBBING
} FwRenderState;

struct _FwCache {
  GObject        parent_instance;

  FwDocument    *document;    /* owned reference */
  GHashTable    *pages;       /* int → CacheEntry* (Tier 2: rendered surfaces) */
  GHashTable    *parsed;      /* int → ParsedEntry* (Tier 1: parsed page handles) */
  GHashTable    *thumbs;      /* int → ThumbEntry* (Tier 0: persistent previews) */
  GThreadPool   *pool;
  GThreadPool   *thumb_pool;  /* separate low-priority pool for thumbnail renders */
  GMutex         lock;

  double         zoom;
  int            rotation;
  int            page_count;
  guint          render_gen;  /* bumped when surfaces need re-rendering (zoom/rot/scale) */
  guint          cancel_gen;  /* bumped to abort in-flight jobs (scrubbing/stop) */
  int            scale_factor; /* device pixel ratio for HiDPI rendering */

  /* Velocity and queuing */
  double         velocity;
  FwRenderState  render_state;

  /* Priority state */
  int           *priority_order;  /* page indices in render priority order */
  int            priority_len;

  GtkWidget     *view_widget;  /* owned ref for scheduling redraws */

  gboolean       stopping;     /* TRUE once dispose begins — blocks new jobs */
  gboolean       redraw_scheduled; /* a coalesced redraw idle is already queued */

  /* Bytes-aware Tier 2 cap (Phase 11 Tier 1). Replaces the v1.3.3 fixed
   * 30-page eviction bound — page-count caps mis-fit by orders of
   * magnitude when surface size varies (a 10-page comic at fit-page is
   * ~30 MB; a poster at 400% zoom is ~30 MB *per page*). Eviction only
   * fires when total_cached_bytes > byte_cap, and prefers pages outside
   * the priority window. Pages in the priority window are never evicted
   * — for absurd zooms that exceed cap with just the visible band,
   * slicing (Phase 11 Tier 2) is the proper fix. */
  gsize          total_cached_bytes;
  gsize          byte_cap;
};

G_DEFINE_FINAL_TYPE (FwCache, fw_cache, G_TYPE_OBJECT)

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Total byte cost of a cairo image surface — stride covers row alignment
 * (cairo aligns to 4-byte boundaries for ARGB32, so stride may exceed
 * 4*width by a few bytes per row). Returns 0 for NULL or non-image
 * surfaces. */
static gsize
surface_byte_size (cairo_surface_t *s)
{
  if (!s) return 0;
  if (cairo_surface_get_type (s) != CAIRO_SURFACE_TYPE_IMAGE) return 0;
  int stride = cairo_image_surface_get_stride (s);
  int height = cairo_image_surface_get_height (s);
  if (stride <= 0 || height <= 0) return 0;
  return (gsize) stride * (gsize) height;
}

/* Free one ZoomSlot in place — texture before surface, same lifecycle
 * rule as the entry path. Caller is responsible for byte accounting. */
static void
zoom_slot_clear (ZoomSlot *slot)
{
  if (slot->texture) {
    g_object_unref (slot->texture);
    slot->texture = NULL;
  }
  if (slot->surface) {
    cairo_surface_destroy (slot->surface);
    slot->surface = NULL;
  }
  slot->size_bytes = 0;
}

static void
cache_entry_free (CacheEntry *entry)
{
  /* Drop texture BEFORE surface — the texture holds a reference to the
   * surface's pixel buffer via GBytes. Surface destruction after last
   * texture reference is released is safe.
   *
   * NOTE: this function does NOT update FwCache->total_cached_bytes —
   * it has no access to self. Callers that explicitly remove entries
   * from self->pages must subtract entry->size_bytes + entry->prev_slots_bytes
   * BEFORE the remove. The dispose path resets total_cached_bytes to 0. */
  if (entry->texture)
    g_object_unref (entry->texture);
  if (entry->surface)
    cairo_surface_destroy (entry->surface);
  for (int i = 0; i < entry->prev_slot_count; i++)
    zoom_slot_clear (&entry->prev_slots[i]);
  g_free (entry);
}

static void
thumb_entry_free (ThumbEntry *entry)
{
  if (entry->texture)
    g_object_unref (entry->texture);
  if (entry->surface)
    cairo_surface_destroy (entry->surface);
  g_free (entry);
}

/* Build a GdkTexture that zero-copies the cairo surface's pixel buffer.
 * The surface reference passed to GBytes keeps pixels alive until the
 * texture is destroyed. */
static GdkTexture *
texture_from_surface (cairo_surface_t *surface)
{
  if (!surface)
    return NULL;

  cairo_surface_flush (surface);
  int sw = cairo_image_surface_get_width (surface);
  int sh = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  unsigned char *data = cairo_image_surface_get_data (surface);
  gsize data_size = (gsize) stride * (gsize) sh;

  cairo_surface_reference (surface);  /* GBytes holds this */
  GBytes *bytes = g_bytes_new_with_free_func (
    data, data_size, (GDestroyNotify) cairo_surface_destroy, surface);

  GdkTexture *tex = GDK_TEXTURE (
    gdk_memory_texture_new (sw, sh,
                            GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                            bytes, (gsize) stride));
  g_bytes_unref (bytes);
  return tex;
}

/* ParsedEntry free requires the document to close the page handle.
 * Since we can't pass document into the GDestroyNotify, parsed entries
 * are freed manually via parsed_entry_free_with_doc(). */
static void
parsed_entry_free_with_doc (ParsedEntry *entry, FwDocument *doc)
{
  if (entry->handle)
    fw_document_close_page (doc, entry->handle);
  g_free (entry);
}

static CacheEntry *
get_or_create_entry (FwCache *self, int page)
{
  CacheEntry *entry = g_hash_table_lookup (self->pages,
                                            GINT_TO_POINTER (page));
  if (!entry) {
    entry = g_new0 (CacheEntry, 1);
    g_hash_table_insert (self->pages, GINT_TO_POINTER (page), entry);
  }
  return entry;
}

/* Tier 2 byte cap. Total surface + prev_surface bytes across all cached
 * pages. When exceeded, eviction drops outside-priority pages by oldest
 * cache-table-iteration order until under cap. Sized for a typical
 * 16 GB Linux laptop running with several other GUI apps; a comic at
 * fit-width fits ~25 pages here, a textbook at fit-width fits ~150. */
#define CACHE_BYTES_CAP_DEFAULT (512u * 1024u * 1024u)

/* Per-render surface allocation cap. A render whose surface would exceed
 * this (extreme zoom on a poster PDF) is scaled down to fit and GTK
 * upscales the texture on paint — see render_worker for the rationale. */
#define RENDER_BYTES_CAP (64u * 1024u * 1024u)

/* Upper bound on the priority_order array. Priority is visible (~1-3
 * pages) plus NEAR_RANGE forward and NEAR_RANGE backward — fixed at
 * compile time, so the array can be sized once per `fw_cache_set_priority`
 * call. 64 is generous safety margin against unusual viewport sizes. */
#define MAX_PRIORITY_PAGES 64

/* ── Redraw helper ───────────────────────────────────────────────── */

/* Called on the main thread via g_idle_add. Holds a ref on the cache so
 * it stays alive until the idle fires (which therefore always runs
 * before dispose); clears the coalescing flag, then queues a draw on the
 * view widget if it's still realized. The widget may already be
 * disposed/unrealized during a document swap — check before touching it. */
static gboolean
safe_queue_draw (gpointer user_data)
{
  FwCache *self = user_data;

  g_mutex_lock (&self->lock);
  self->redraw_scheduled = FALSE;
  GtkWidget *widget = self->view_widget;
  if (widget) g_object_ref (widget);   /* keep alive past the unlock */
  g_mutex_unlock (&self->lock);

  if (widget) {
    if (GTK_IS_WIDGET (widget) && gtk_widget_get_parent (widget) != NULL)
      gtk_widget_queue_draw (widget);
    g_object_unref (widget);
  }
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

/* Queue a coalesced main-thread redraw. Caller holds self->lock. A render
 * burst (e.g. 14 pages at open) would otherwise schedule one idle source
 * per stored page; the redraw_scheduled flag collapses them to one. */
static void
schedule_redraw (FwCache *self)
{
  if (!self->view_widget || self->redraw_scheduled)
    return;
  self->redraw_scheduled = TRUE;
  g_idle_add (safe_queue_draw, g_object_ref (self));
}

/* ── Thread pool worker ───────────────────────────────────────────── */

static void submit_next_jobs (FwCache *self);

/* GThreadPool sort comparator. Higher last_view_time runs first — newly
 * pushed jobs (current viewport) jump to the front of the queue ahead of
 * older queued jobs from a previous priority list. Pure GLib pattern from
 * zathura's render.c. */
static gint
render_job_compare (gconstpointer a, gconstpointer b, gpointer user_data)
{
  (void) user_data;
  const RenderJob *ja = a;
  const RenderJob *jb = b;
  if (jb->last_view_time > ja->last_view_time) return  1;
  if (jb->last_view_time < ja->last_view_time) return -1;
  return 0;
}

/* LRU eviction comparator — oldest access first so g_array_sort puts
 * the next-to-evict candidate at index 0. */
typedef struct { int page; gint64 access_us; } LruVictim;
static gint
lru_victim_compare (gconstpointer a, gconstpointer b)
{
  const LruVictim *va = a;
  const LruVictim *vb = b;
  if (va->access_us < vb->access_us) return -1;
  if (va->access_us > vb->access_us) return  1;
  return 0;
}

/* Linear membership test against the current priority window. The window
 * is at most MAX_PRIORITY_PAGES (64) entries, so a scan beats building a
 * GHashTable keep-set on every fw_cache_set_priority call — and that runs
 * on every scroll tick. Caller holds self->lock. */
static gboolean
page_in_priority (FwCache *self, int page)
{
  for (int i = 0; i < self->priority_len; i++)
    if (self->priority_order[i] == page)
      return TRUE;
  return FALSE;
}

/* ── Thumbnail worker ─────────────────────────────────────────────── */

typedef struct {
  FwCache *cache;
  int      page;
  double   zoom;
} ThumbJob;

static void
thumb_worker (gpointer data, gpointer user_data)
{
  (void) user_data;
  ThumbJob *job = data;
  FwCache *self = job->cache;

  g_mutex_lock (&self->lock);
  if (self->stopping) {
    g_mutex_unlock (&self->lock);
    g_free (job);
    return;
  }
  g_mutex_unlock (&self->lock);

  FW_TRACE_CACHE ("thumb render: page=%d zoom=%.3f", job->page, job->zoom);
  cairo_surface_t *surface = fw_document_render_page (
    self->document, job->page, job->zoom, 0);

  g_mutex_lock (&self->lock);
  ThumbEntry *te = g_hash_table_lookup (self->thumbs, GINT_TO_POINTER (job->page));
  if (!te) {
    /* thumbs table was cleared while we rendered */
    if (surface) cairo_surface_destroy (surface);
    g_mutex_unlock (&self->lock);
    g_free (job);
    return;
  }

  if (surface) {
    te->surface = surface;
    te->texture = texture_from_surface (surface);
    te->rendering = FALSE;
    FW_TRACE_MEM ("thumb stored: page=%d surface=%p", job->page, (void *) surface);
    schedule_redraw (self);
  } else {
    te->rendering = FALSE;
    te->failed = TRUE;
  }
  g_mutex_unlock (&self->lock);
  g_free (job);
}

static void
render_worker (gpointer data, gpointer user_data)
{
  (void) user_data;
  RenderJob *job = data;
  FwCache *self = job->cache;

  FW_TRACE_CACHE ("worker start: page=%d rgen=%u cgen=%u zoom=%.2f",
                  job->page, job->render_gen, job->cancel_gen, job->zoom);

  /* Check if this job is still relevant.
   * - cancel_gen mismatch: scrubbing or stop happened — abort immediately
   * - render_gen mismatch: zoom/rotation changed — surface would be wrong size
   * - scrubbing state: don't start new work while user is flinging */
  g_mutex_lock (&self->lock);
  gboolean cancelled = (job->cancel_gen != self->cancel_gen);
  gboolean wrong_params = (job->render_gen != self->render_gen);
  gboolean scrubbing = (self->render_state == FW_RENDER_STATE_SCRUBBING);
  if (cancelled || wrong_params || scrubbing) {
    FW_TRACE_CACHE ("worker skip (cancel=%d params=%d scrub=%d): page=%d",
                    cancelled, wrong_params, scrubbing, job->page);
    CacheEntry *entry = g_hash_table_lookup (self->pages, GINT_TO_POINTER (job->page));
    if (entry) entry->rendering = FALSE;
    submit_next_jobs (self);
    g_mutex_unlock (&self->lock);
    g_free (job);
    return;
  }
  g_mutex_unlock (&self->lock);

  /* Try to find or create a cached page handle (display list / parsed page).
   * Display lists are created lazily here in the worker thread, NOT on the
   * main thread — this prevents complex pages from blocking the UI. */
  gpointer parsed_handle = NULL;
  g_mutex_lock (&self->lock);
  ParsedEntry *parsed = g_hash_table_lookup (self->parsed,
                                              GINT_TO_POINTER (job->page));
  if (parsed && parsed->handle)
    parsed_handle = parsed->handle;
  g_mutex_unlock (&self->lock);

  /* If no cached handle, create one now (off the main thread) */
  if (!parsed_handle) {
    FW_TRACE_CACHE ("io: open_page %d (cache miss)", job->page);
    gpointer new_handle = fw_document_open_page (self->document, job->page);
    if (new_handle) {
      g_mutex_lock (&self->lock);
      /* Check again — another worker may have beaten us */
      parsed = g_hash_table_lookup (self->parsed,
                                     GINT_TO_POINTER (job->page));
      if (parsed && parsed->handle) {
        /* Someone else created it — use theirs, drop ours */
        parsed_handle = parsed->handle;
        fw_document_close_page (self->document, new_handle);
      } else {
        /* Cache our new handle */
        ParsedEntry *pe = g_new0 (ParsedEntry, 1);
        pe->handle     = new_handle;
        pe->render_gen  = job->render_gen;
        g_hash_table_insert (self->parsed,
                              GINT_TO_POINTER (job->page), pe);
        parsed_handle = new_handle;
      }
      g_mutex_unlock (&self->lock);
    }
  }

  /* Bytes-cap the per-render allocation so an extreme zoom doesn't
   * blow the heap on a poster-format PDF. At RENDER_BYTES_CAP the
   * render can't exceed 64 MB / surface; if the requested zoom would
   * produce more, scale `render_zoom` down so the surface fits. The
   * resulting texture then upscales via GTK's GSK pipeline when the
   * view paints it at the requested rect — try_closest_rendered_page
   * (v0.28) treats the capped slot the same as any other prev-zoom
   * slot, so the user just sees a slightly-blurry preview at extreme
   * zoom rather than an OOM.
   *
   * Phase 11 Tier 2 originally called for true tile slicing (render
   * the page in N×M cairo surfaces). The cap-and-upscale variant
   * here ships the same memory bound in ~10 LOC vs. several hundred
   * for a real slice path through the cache + view; on the actual
   * Calibre corpus (no poster PDFs) the threshold is essentially
   * never reached, so the visual cost is hypothetical. Slicing is
   * tracked as a follow-up if a poster-format need ever surfaces.
   * (RENDER_BYTES_CAP is defined at file scope with the other caps.) */
  double render_zoom = job->zoom * job->scale_factor;
  double effective_doc_zoom = job->zoom;
  {
    double pw, ph;
    fw_document_get_page_size (self->document, job->page, &pw, &ph);
    if (pw > 0 && ph > 0) {
      double bytes = pw * render_zoom * ph * render_zoom * 4.0;
      if (bytes > (double) RENDER_BYTES_CAP) {
        double scale_down = sqrt ((double) RENDER_BYTES_CAP / bytes);
        render_zoom        *= scale_down;
        effective_doc_zoom *= scale_down;
        FW_TRACE_CACHE ("render-cap: page=%d req_zoom=%.3f → eff=%.3f",
                        job->page, job->zoom, effective_doc_zoom);
      }
    }
  }

  /* Render the page (this is the expensive part — runs unlocked) */
  cairo_surface_t *surface;
  if (parsed_handle)
    surface = fw_document_render_page_from_handle (
      self->document, parsed_handle, render_zoom, job->rotation);
  else
    surface = fw_document_render_page (
      self->document, job->page, render_zoom, job->rotation);

  /* Store result */
  g_mutex_lock (&self->lock);

  /* If the render returned NULL because cancel_gen was bumped during
   * the render (PDF fz_cookie abort, CBR/DjVu cancel_gen bump), treat it
   * as a transient cancellation — clear `rendering` but DON'T mark
   * `render_gen=current`. Otherwise the v0.24.0 sticky-fail check in
   * `submit_next_jobs` would skip this page until the next zoom or
   * rotation change, leaving it stuck on the thumbnail tier (the
   * "page stays blurred until I zoom" symptom).
   *
   * Distinguishing "cancellation NULL" from "deterministic-failure
   * NULL" (e.g. CBR's zero-size render): we look at whether the
   * cancel generation was bumped during the render. Bumped → cancel,
   * retry on next priority push. Unbumped → real failure, sticky. */
  if (surface == NULL && job->cancel_gen != self->cancel_gen
      && job->render_gen == self->render_gen) {
    FW_TRACE_CACHE ("worker cancelled mid-render: page=%d (no sticky fail)",
                    job->page);
    CacheEntry *entry = g_hash_table_lookup (self->pages,
                                              GINT_TO_POINTER (job->page));
    if (entry) entry->rendering = FALSE;
    submit_next_jobs (self);
    g_mutex_unlock (&self->lock);
    g_free (job);
    return;
  }

  if (job->render_gen == self->render_gen) {
    /* Surface was rendered with correct zoom/rotation — keep it.
     * Even if cancel_gen changed (user scrolled), the surface is still valid
     * for display since the render parameters haven't changed.
     *
     * Multi-zoom retention: prev_slots are intentionally NOT cleared
     * here — they were demoted at fw_cache_start time and serve as
     * "closer than thumbnail" placeholders until evicted by zoom-cap
     * shifting or by the bytes-cap LRU pass. */
    FW_TRACE_CACHE ("worker done: page=%d surface=%p", job->page, (void *) surface);
    CacheEntry *entry = get_or_create_entry (self, job->page);
    /* Replace current surface and its cached texture */
    if (entry->texture) {
      g_object_unref (entry->texture);
      entry->texture = NULL;
    }
    if (entry->surface)
      cairo_surface_destroy (entry->surface);
    self->total_cached_bytes -= entry->size_bytes;
    entry->surface       = surface;
    entry->texture       = texture_from_surface (surface);
    entry->size_bytes    = surface_byte_size (surface);
    entry->zoom          = effective_doc_zoom;
    entry->rotation      = job->rotation;
    entry->scale_factor  = job->scale_factor;
    self->total_cached_bytes += entry->size_bytes;
    entry->rendering  = FALSE;
    entry->render_gen = job->render_gen;
    entry->last_access_us = g_get_monotonic_time ();

    /* Schedule a coalesced redraw on the main thread so the new surface
     * appears (see schedule_redraw / safe_queue_draw). */
    schedule_redraw (self);
  } else {
    /* Render params changed (zoom/rotation) — surface is wrong size, discard */
    FW_TRACE_CACHE ("worker stale-discard: page=%d rgen=%u (current=%u) surface=%p",
                    job->page, job->render_gen, self->render_gen, (void *) surface);
    if (surface)
      cairo_surface_destroy (surface);
    CacheEntry *entry = g_hash_table_lookup (self->pages,
                                              GINT_TO_POINTER (job->page));
    if (entry) entry->rendering = FALSE;
  }

  submit_next_jobs (self);
  g_mutex_unlock (&self->lock);

  g_free (job);
}

static void
submit_next_jobs (FwCache *self)
{
  if (self->stopping || self->render_state == FW_RENDER_STATE_SCRUBBING)
    return;

  /* Push every unrendered in-window page at once. The pool's sort function
   * (render_job_compare) reorders by last_view_time so the most-recently-
   * prioritized page runs next; the pool's own worker count caps concurrency.
   * Earlier indices in priority_order have a slightly later timestamp so
   * visible pages still beat near-buffer pages within a single push. */
  gint64 base_time = g_get_monotonic_time ();
  for (int i = 0; i < self->priority_len; i++) {
    int pg = self->priority_order[i];
    CacheEntry *entry = get_or_create_entry (self, pg);
    if (entry->rendering)
      continue;
    /* Skip if this entry has already been rendered at the current
     * generation — even if the render returned NULL. The previous
     * `entry->surface && ...` check meant a failing render
     * (e.g., the CBR backend's "zero-size render" for thumbnail-tier
     * pages) would re-queue forever, since each retry produced another
     * NULL and another retry. Sticky-fail until the next gen bump. */
    if (entry->render_gen == self->render_gen)
      continue;

    entry->rendering = TRUE;

    RenderJob *job = g_new0 (RenderJob, 1);
    job->cache          = self;
    job->page           = pg;
    job->zoom           = self->zoom;
    job->rotation       = self->rotation;
    job->scale_factor   = self->scale_factor;
    job->render_gen     = self->render_gen;
    job->cancel_gen     = self->cancel_gen;
    /* Subtract the index so earlier-in-priority pages have a later effective
     * timestamp than later-in-priority pages submitted in this same call. */
    job->last_view_time = base_time - i;

    g_thread_pool_push (self->pool, job, NULL);
  }
}

/* ── Public API ───────────────────────────────────────────────────── */

FwCache *
fw_cache_new (FwDocument *document, GtkWidget *view_widget)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (document), NULL);

  FwCache *self = g_object_new (FW_TYPE_CACHE, NULL);
  self->document     = g_object_ref (document);
  self->page_count   = fw_document_get_page_count (document);
  self->view_widget  = view_widget ? g_object_ref (view_widget) : NULL;
  self->scale_factor = 1;
  return self;
}

void
fw_cache_start (FwCache *self, double zoom, int rotation)
{
  g_return_if_fail (FW_IS_CACHE (self));

  FW_TRACE_CACHE ("start: zoom=%.2f rot=%d", zoom, rotation);
  g_mutex_lock (&self->lock);

  self->zoom     = zoom;
  self->rotation = rotation;
  self->render_gen++;
  self->cancel_gen++;

  /* Do NOT clear existing cache — keep old surfaces visible while new
   * ones render. Demote current surface to prev_slots[0] (shifting
   * older slots right; oldest evicts at MAX_PREV_ZOOM_SLOTS) so a
   * scaled placeholder is available during the re-render transition.
   * With multi-slot retention, fw_cache_get_texture later picks the
   * slot whose zoom is closest to the new self->zoom — Sumatra/Sioyek
   * try_closest_rendered_page pattern. */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->pages);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    CacheEntry *entry = value;
    entry->rendering = FALSE;  /* allow re-submission */
    if (!entry->surface || entry->render_gen == self->render_gen)
      continue;

    /* Evict the oldest slot if we're at cap. */
    if (entry->prev_slot_count == MAX_PREV_ZOOM_SLOTS) {
      ZoomSlot *oldest = &entry->prev_slots[MAX_PREV_ZOOM_SLOTS - 1];
      self->total_cached_bytes -= oldest->size_bytes;
      entry->prev_slots_bytes  -= oldest->size_bytes;
      zoom_slot_clear (oldest);
      entry->prev_slot_count = MAX_PREV_ZOOM_SLOTS - 1;
    }
    /* Shift slots right to make room at index 0. */
    for (int i = entry->prev_slot_count; i > 0; i--)
      entry->prev_slots[i] = entry->prev_slots[i - 1];
    /* Demote the current surface into the freed slot 0. Bytes stay
     * counted in total_cached_bytes — they just move from
     * size_bytes to prev_slots_bytes. */
    entry->prev_slots[0] = (ZoomSlot){
      .surface      = entry->surface,
      .texture      = entry->texture,
      .zoom         = entry->zoom,
      .rotation     = entry->rotation,
      .scale_factor = entry->scale_factor,
      .size_bytes   = entry->size_bytes,
    };
    entry->prev_slot_count++;
    entry->prev_slots_bytes += entry->size_bytes;
    entry->surface    = NULL;
    entry->texture    = NULL;
    entry->size_bytes = 0;
    FW_TRACE_MEM ("zoom_slot demote: page=%d zoom=%.2f slot_count=%d",
                  GPOINTER_TO_INT (key), entry->prev_slots[0].zoom,
                  entry->prev_slot_count);
  }

  /* Submit only pages in the priority window.  If no priority has been
   * set yet (first open), submit the first few pages so there
   * is something to show immediately. */
  if (self->priority_order && self->priority_len > 0) {
    FW_TRACE_CACHE ("start: using existing priority (len=%d, first=%d) rgen=%u",
                    self->priority_len, self->priority_order[0], self->render_gen);
    submit_next_jobs (self);
  } else {
    int n = self->page_count < 14 ? self->page_count : 14;
    self->priority_order = g_new (int, n);
    for (int i = 0; i < n; i++)
      self->priority_order[i] = i;
    self->priority_len = n;
    FW_TRACE_CACHE ("start: default priority pages 0..%d rgen=%u", n - 1, self->render_gen);
    submit_next_jobs (self);
  }

  g_mutex_unlock (&self->lock);
}

void
fw_cache_set_priority (FwCache *self, const int *visible_pages, int n_visible)
{
  g_return_if_fail (FW_IS_CACHE (self));

  g_mutex_lock (&self->lock);

  g_free (self->priority_order);

  if (n_visible <= 0 || !visible_pages) {
    self->priority_order = NULL;
    self->priority_len   = 0;
    g_mutex_unlock (&self->lock);
    return;
  }

  int first_visible = visible_pages[0];
  int last_visible  = visible_pages[n_visible - 1];
  int total = self->page_count;

  /* Symmetric ±NEAR_RANGE preload window. The pool's sort function (by
   * last_view_time) ensures workers pick the most recently prioritized page
   * first, so we don't need a tiered "near vs far" split or throttling — the
   * cache picks visible-first naturally as new priority pushes always carry
   * a fresher timestamp than older queued jobs.
   *
   * NEAR_RANGE is the per-side preload count. Total window = visible +
   * 2*NEAR_RANGE, clamped to MAX_PRIORITY_PAGES as an array-size guard. */
  #define NEAR_RANGE 10

  int window = MAX_PRIORITY_PAGES < total ? MAX_PRIORITY_PAGES : total;
  self->priority_order = g_new (int, window);
  int idx = 0;

  /* 1. Visible pages first */
  for (int i = 0; i < n_visible && idx < window; i++)
    self->priority_order[idx++] = visible_pages[i];

  /* 2. Symmetric ±NEAR_RANGE around the visible band, interleaved so
   * forward and backward fill at equal pace. */
  for (int step = 1; step <= NEAR_RANGE && idx < window; step++) {
    int fwd = last_visible + step;
    int bwd = first_visible - step;
    if (fwd < total && idx < window)
      self->priority_order[idx++] = fwd;
    if (bwd >= 0 && idx < window)
      self->priority_order[idx++] = bwd;
  }

  self->priority_len = idx;

  /* ── Tier 1: Evict parsed page handles outside the priority window ──
   * Handles are created lazily by render workers — we only evict here.
   * IMPORTANT: skip pages with rendering=TRUE — a worker thread may be
   * holding the handle pointer between releasing self->lock and calling
   * render_page_from_handle. Evicting now would free it mid-use.
   * Free-then-steal during iteration: the parsed table has no
   * GDestroyNotify (handles need the document to close), so we free
   * manually then steal the current entry — safe inside the iter. */
  {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, self->parsed);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
      if (page_in_priority (self, GPOINTER_TO_INT (key)))
        continue;
      CacheEntry *ce = g_hash_table_lookup (self->pages, key);
      if (ce && ce->rendering)
        continue;  /* worker may be using this handle — skip */
      parsed_entry_free_with_doc (value, self->document);
      g_hash_table_iter_steal (&iter);
    }
  }

  /* ── Tier 2: Evict rendered surfaces by byte cap ──
   * Outside-priority surfaces are KEPT in cache up to byte_cap so that
   * scrolling back into a previously-viewed area is instant. Eviction
   * fires only when total_cached_bytes > byte_cap, and prefers
   * outside-priority pages (priority pages are never evicted — they're
   * about to be looked at). Pages with in-flight render jobs are also
   * skipped: the worker's `entry` lookup at the store site would race
   * with eviction otherwise. */
  if (self->total_cached_bytes > self->byte_cap) {
    /* TTL+LRU hybrid: sort outside-priority candidates by oldest
     * `last_access_us` and evict from the front. The previous
     * iteration-order eviction was effectively arbitrary; LRU
     * preserves the most recently scrolled-back-to pages, which is
     * what the user expects when they reverse direction. */
    GArray *victims = g_array_new (FALSE, FALSE, sizeof (LruVictim));

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, self->pages);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
      if (page_in_priority (self, GPOINTER_TO_INT (key))) continue;
      CacheEntry *entry = value;
      if (entry->rendering) continue;
      LruVictim v = { .page = GPOINTER_TO_INT (key),
                      .access_us = entry->last_access_us };
      g_array_append_val (victims, v);
    }
    g_array_sort (victims, lru_victim_compare);

    gsize freed = 0;
    guint dropped = 0;
    for (guint i = 0; i < victims->len; i++) {
      if (self->total_cached_bytes <= self->byte_cap) break;
      int pg = g_array_index (victims, LruVictim, i).page;
      CacheEntry *e = g_hash_table_lookup (self->pages, GINT_TO_POINTER (pg));
      if (!e) continue;
      gsize entry_bytes = e->size_bytes + e->prev_slots_bytes;
      self->total_cached_bytes -= entry_bytes;
      freed += entry_bytes;
      dropped++;
      g_hash_table_remove (self->pages, GINT_TO_POINTER (pg));
    }
    g_array_unref (victims);
    FW_TRACE_MEM ("byte-cap evict: dropped=%u freed=%zu remaining=%zu cap=%zu",
                  dropped, freed, self->total_cached_bytes, self->byte_cap);
  }

  FW_TRACE_MEM ("cache after eviction: surfaces=%u parsed=%u priority=%d bytes=%zu",
                g_hash_table_size (self->pages),
                g_hash_table_size (self->parsed),
                self->priority_len,
                self->total_cached_bytes);

  /* Submit renders for pages in the window */
  submit_next_jobs (self);

  g_mutex_unlock (&self->lock);
}

gboolean
fw_cache_set_velocity (FwCache *self, double velocity)
{
  g_return_val_if_fail (FW_IS_CACHE (self), FALSE);

  g_mutex_lock (&self->lock);

  self->velocity = velocity;
  FwRenderState new_state;
  if (velocity > 1.5)
    new_state = FW_RENDER_STATE_SCRUBBING;
  else if (velocity > 0.2)
    new_state = FW_RENDER_STATE_CRUISING;
  else
    new_state = FW_RENDER_STATE_STATIC;

  gboolean state_changed = (self->render_state != new_state);

  if (state_changed) {
    FW_TRACE_CACHE ("velocity %.2f → state %s",
                    velocity,
                    new_state == FW_RENDER_STATE_SCRUBBING ? "SCRUBBING" :
                    new_state == FW_RENDER_STATE_CRUISING  ? "CRUISING"  : "STATIC");
    self->render_state = new_state;

    if (new_state == FW_RENDER_STATE_SCRUBBING) {
      /* High-Velocity Abort: cancel in-flight jobs but DON'T invalidate
       * already-rendered surfaces — zoom hasn't changed, they're still valid */
      self->cancel_gen++;
      /* Notify backend to bail out of any in-progress render */
      fw_document_cancel_render (self->document);
    } else {
      /* Submit jobs again if we dropped back to cruising or static */
      submit_next_jobs (self);
    }
  }

  g_mutex_unlock (&self->lock);
  return state_changed;
}

cairo_surface_t *
fw_cache_get_page (FwCache *self, int page)
{
  g_return_val_if_fail (FW_IS_CACHE (self), NULL);

  g_mutex_lock (&self->lock);
  CacheEntry *entry = g_hash_table_lookup (self->pages,
                                            GINT_TO_POINTER (page));
  cairo_surface_t *surface = NULL;
  if (entry && entry->surface) {
    surface = cairo_surface_reference (entry->surface);
    entry->last_access_us = g_get_monotonic_time ();
  }
  g_mutex_unlock (&self->lock);

  return surface;
}

gboolean
fw_cache_page_ready (FwCache *self, int page)
{
  g_return_val_if_fail (FW_IS_CACHE (self), FALSE);

  g_mutex_lock (&self->lock);
  CacheEntry *entry = g_hash_table_lookup (self->pages,
                                            GINT_TO_POINTER (page));
  gboolean ready = (entry && entry->surface != NULL);
  g_mutex_unlock (&self->lock);

  return ready;
}

GdkTexture *
fw_cache_get_texture (FwCache *self, int page)
{
  g_return_val_if_fail (FW_IS_CACHE (self), NULL);

  g_mutex_lock (&self->lock);
  CacheEntry *entry = g_hash_table_lookup (self->pages,
                                            GINT_TO_POINTER (page));
  /* Prefer current-gen texture. When unavailable, return the
   * previous-zoom slot whose zoom is closest to the current cache
   * zoom (matching rotation+scale_factor) — Sioyek's
   * try_closest_rendered_page idiom. GTK auto-scales the texture into
   * whatever rect the view paints into, so a slot rendered at 2.4×
   * shown at 2.5× is much sharper than one rendered at 1.0×. Returns
   * a borrowed reference — caller must not unref. */
  GdkTexture *tex = NULL;
  if (entry) {
    if (entry->texture && entry->render_gen == self->render_gen) {
      tex = entry->texture;
    } else if (entry->prev_slot_count > 0) {
      double target = self->zoom;
      double best_diff = -1;
      const ZoomSlot *best = NULL;
      for (int i = 0; i < entry->prev_slot_count; i++) {
        const ZoomSlot *s = &entry->prev_slots[i];
        if (!s->texture) continue;
        if (s->rotation != self->rotation) continue;
        if (s->scale_factor != self->scale_factor) continue;
        double diff = fabs (s->zoom - target);
        if (best_diff < 0 || diff < best_diff) {
          best_diff = diff;
          best = s;
        }
      }
      if (best)
        tex = best->texture;
    }
    if (tex)
      entry->last_access_us = g_get_monotonic_time ();
  }
  g_mutex_unlock (&self->lock);
  return tex;
}

GdkTexture *
fw_cache_get_thumbnail (FwCache *self, int page, double page_w, double page_h)
{
  g_return_val_if_fail (FW_IS_CACHE (self), NULL);

  /* page_count is immutable after construction; page_w/h are args — safe
   * to check before locking. `stopping` is mutable (set under lock in
   * fw_cache_stop), so its check moves inside the lock. */
  if (page < 0 || page >= self->page_count)
    return NULL;
  if (page_w <= 0 || page_h <= 0)
    return NULL;

  g_mutex_lock (&self->lock);
  if (self->stopping) {
    g_mutex_unlock (&self->lock);
    return NULL;
  }
  ThumbEntry *te = g_hash_table_lookup (self->thumbs, GINT_TO_POINTER (page));

  if (te && te->texture) {
    GdkTexture *tex = te->texture;
    g_mutex_unlock (&self->lock);
    return tex;
  }

  if (te && (te->rendering || te->failed)) {
    g_mutex_unlock (&self->lock);
    return NULL;
  }

  /* Create entry and kick off a thumbnail render */
  if (!te) {
    te = g_new0 (ThumbEntry, 1);
    g_hash_table_insert (self->thumbs, GINT_TO_POINTER (page), te);
  }
  te->rendering = TRUE;

  double zoom = THUMB_WIDTH / page_w;
  if (zoom <= 0 || zoom > 1.0) zoom = 0.25;

  ThumbJob *job = g_new0 (ThumbJob, 1);
  job->cache = self;
  job->page  = page;
  job->zoom  = zoom;

  if (self->thumb_pool)
    g_thread_pool_push (self->thumb_pool, job, NULL);
  else
    g_free (job);

  g_mutex_unlock (&self->lock);
  return NULL;
}

void
fw_cache_set_scale_factor (FwCache *self, int scale_factor)
{
  g_return_if_fail (FW_IS_CACHE (self));
  if (scale_factor < 1) scale_factor = 1;

  g_mutex_lock (&self->lock);
  if (self->scale_factor != scale_factor) {
    self->scale_factor = scale_factor;
    /* Invalidate all rendered surfaces — they're at the wrong resolution */
    self->render_gen++;
    self->cancel_gen++;
  }
  g_mutex_unlock (&self->lock);
}

void
fw_cache_stop (FwCache *self)
{
  g_return_if_fail (FW_IS_CACHE (self));

  FW_TRACE_CACHE ("stop%s", "");
  g_mutex_lock (&self->lock);
  self->stopping = TRUE;
  self->cancel_gen++;  /* invalidate all pending jobs */
  g_mutex_unlock (&self->lock);

  /* Tell the backend to bail out of any in-progress render (e.g. DjVu
   * page decode).  Without this, g_thread_pool_free blocks the main
   * thread until the decode finishes — potentially seconds. */
  fw_document_cancel_render (self->document);
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_cache_dispose (GObject *object)
{
  FwCache *self = FW_CACHE (object);
  FW_TRACE_MEM ("cache dispose enter: stopping=%d", self->stopping);

  fw_cache_stop (self);

  /* Drain pending jobs before freeing the pool (immediate=FALSE) so
   * each queued RenderJob runs through its worker — which sees
   * cancel_gen has been bumped, bails out, and g_free's the job.
   * Using immediate=TRUE here would drop queued jobs without invoking
   * the worker, leaking every pending RenderJob struct. The
   * fw_document_cancel_render() call in fw_cache_stop ensures
   * mid-render decodes bail quickly so this drain doesn't block. */
  if (self->pool) {
    g_thread_pool_free (self->pool, FALSE, TRUE);
    self->pool = NULL;
  }
  if (self->thumb_pool) {
    g_thread_pool_free (self->thumb_pool, FALSE, TRUE);
    self->thumb_pool = NULL;
  }

  /* Free all parsed page handles before dropping the document */
  FW_TRACE_MEM ("cache dispose: parsed=%u surfaces=%u doc=%p",
                self->parsed ? g_hash_table_size (self->parsed) : 0,
                self->pages  ? g_hash_table_size (self->pages)  : 0,
                (void *) self->document);
  if (self->parsed && self->document) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, self->parsed);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
      parsed_entry_free_with_doc (value, self->document);
    }
    g_hash_table_remove_all (self->parsed);
  }

  g_clear_object (&self->document);

  if (self->view_widget) {
    g_object_unref (self->view_widget);
    self->view_widget = NULL;
  }

  G_OBJECT_CLASS (fw_cache_parent_class)->dispose (object);
}

static void
fw_cache_finalize (GObject *object)
{
  FwCache *self = FW_CACHE (object);

  g_hash_table_unref (self->pages);
  g_hash_table_unref (self->parsed);
  g_hash_table_unref (self->thumbs);
  g_mutex_clear (&self->lock);
  g_free (self->priority_order);

  G_OBJECT_CLASS (fw_cache_parent_class)->finalize (object);
}

static void
fw_cache_class_init (FwCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose  = fw_cache_dispose;
  object_class->finalize = fw_cache_finalize;
}

static void
fw_cache_init (FwCache *self)
{
  g_mutex_init (&self->lock);

  self->pages = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                        NULL,
                                        (GDestroyNotify) cache_entry_free);

  /* Tier 1: parsed page handles. No GDestroyNotify because we need
   * the document pointer to close handles — freed manually. */
  self->parsed = g_hash_table_new (g_direct_hash, g_direct_equal);

  /* Tier 0: persistent low-resolution previews */
  self->thumbs = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                         NULL,
                                         (GDestroyNotify) thumb_entry_free);

  /* Thread pool: use number of processors, non-exclusive so threads
   * are shared across pools. */
  int n_threads = (int) g_get_num_processors ();
  if (n_threads < 2) n_threads = 2;
  if (n_threads > 8) n_threads = 8;

  self->pool = g_thread_pool_new (render_worker, NULL, n_threads, FALSE, NULL);
  /* Sort by last_view_time so the most-recently-prioritized page runs
   * next, regardless of when its job was pushed. New high-priority pushes
   * (current viewport) jump ahead of older queued jobs. */
  g_thread_pool_set_sort_function (self->pool, render_job_compare, NULL);

  /* Single dedicated thread for thumbnails — background priority, never
   * competes with the main render pool. */
  self->thumb_pool = g_thread_pool_new (thumb_worker, NULL, 1, FALSE, NULL);
  self->render_state = FW_RENDER_STATE_STATIC;
  self->scale_factor = 1;
  self->total_cached_bytes = 0;
  /* Honour FW_CACHE_BYTES_CAP_MB env var for tuning and testing — falls
   * back to the compiled-in default if unset or unparseable. */
  const char *cap_env = g_getenv ("FW_CACHE_BYTES_CAP_MB");
  if (cap_env) {
    char *end = NULL;
    long mb = strtol (cap_env, &end, 10);
    self->byte_cap = (mb > 0) ? (gsize) mb * 1024u * 1024u
                              : CACHE_BYTES_CAP_DEFAULT;
  } else {
    self->byte_cap = CACHE_BYTES_CAP_DEFAULT;
  }
}
