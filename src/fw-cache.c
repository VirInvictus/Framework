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

typedef struct {
  cairo_surface_t *surface;      /* NULL if not yet rendered */
  cairo_surface_t *prev_surface; /* previous-gen surface for zoom placeholder */
  gboolean         rendering;    /* TRUE if a job is in the pool for this page */
  guint            render_gen;   /* render params generation when this surface was created */
} CacheEntry;

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
  guint    render_gen; /* render params generation at time of job creation */
  guint    cancel_gen; /* cancel generation at time of job creation */
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
  GThreadPool   *pool;
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
  int            active_jobs;
  int            max_jobs;

  /* Priority state */
  int           *priority_order;  /* page indices in render priority order */
  int            priority_len;

  GtkWidget     *view_widget;  /* owned ref for scheduling redraws */

  gboolean       stopping;     /* TRUE once dispose begins — blocks new jobs */

  /* Throttle: avoid rebuilding priority on every scroll tick */
  gint64         last_priority_time;  /* monotonic µs of last set_priority */
};

G_DEFINE_FINAL_TYPE (FwCache, fw_cache, G_TYPE_OBJECT)

/* ── Helpers ──────────────────────────────────────────────────────── */

static void
cache_entry_free (CacheEntry *entry)
{
  if (entry->surface)
    cairo_surface_destroy (entry->surface);
  if (entry->prev_surface)
    cairo_surface_destroy (entry->prev_surface);
  g_free (entry);
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

/* Max pages to keep in memory: visible + buffer ahead/behind.
 * At fit-width on 1920px, a typical page surface is ~3-6 MB.
 * 50 pages ≈ 150-300 MB — comfortable on 16 GB RAM. */
#define CACHE_WINDOW 30

/* ── Redraw helper ───────────────────────────────────────────────── */

/* Called on the main thread via g_idle_add.  The widget is ref'd so the
 * GObject is alive, but it may already be disposed/unrealized during
 * document swap — check before touching GTK state. */
static gboolean
safe_queue_draw (gpointer user_data)
{
  GtkWidget *widget = user_data;
  if (GTK_IS_WIDGET (widget) && gtk_widget_get_parent (widget) != NULL)
    gtk_widget_queue_draw (widget);
  g_object_unref (widget);
  return G_SOURCE_REMOVE;
}

/* ── Thread pool worker ───────────────────────────────────────────── */

static void submit_next_jobs (FwCache *self);

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
    self->active_jobs--;
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

  double render_zoom = job->zoom * self->scale_factor;

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
  if (job->render_gen == self->render_gen) {
    /* Surface was rendered with correct zoom/rotation — keep it.
     * Even if cancel_gen changed (user scrolled), the surface is still valid
     * for display since the render parameters haven't changed. */
    FW_TRACE_CACHE ("worker done: page=%d surface=%p", job->page, (void *) surface);
    CacheEntry *entry = get_or_create_entry (self, job->page);
    /* Fresh surface at current generation — drop any stale placeholder */
    if (entry->prev_surface) {
      cairo_surface_destroy (entry->prev_surface);
      entry->prev_surface = NULL;
    }
    if (entry->surface)
      cairo_surface_destroy (entry->surface);
    entry->surface    = surface;
    entry->rendering  = FALSE;
    entry->render_gen = job->render_gen;

    /* Schedule a redraw on the main thread so the new surface appears.
     * We ref the widget so it stays alive until the idle fires — the
     * callback unrefs after checking the widget is still realized. */
    if (self->view_widget)
      g_idle_add (safe_queue_draw, g_object_ref (self->view_widget));
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
  
  self->active_jobs--;
  submit_next_jobs (self);
  g_mutex_unlock (&self->lock);

  g_free (job);
}

static void
submit_next_jobs (FwCache *self)
{
  if (self->stopping || self->render_state == FW_RENDER_STATE_SCRUBBING)
    return;

  /* Limit concurrency based on render state: cruising gets fewer slots
   * to avoid burning CPU on pages that will scroll away before use. */
  int job_limit = self->max_jobs;
  if (self->render_state == FW_RENDER_STATE_CRUISING)
    job_limit = 2;

  while (self->active_jobs < job_limit) {
    int page_to_submit = -1;
    for (int i = 0; i < self->priority_len; i++) {
      int pg = self->priority_order[i];
      CacheEntry *entry = get_or_create_entry (self, pg);
      if (!entry->rendering && (!entry->surface || entry->render_gen != self->render_gen)) {
        page_to_submit = pg;
        break;
      }
    }

    if (page_to_submit == -1)
      break;

    CacheEntry *entry = get_or_create_entry (self, page_to_submit);
    entry->rendering = TRUE;
    self->active_jobs++;

    RenderJob *job = g_new0 (RenderJob, 1);
    job->cache      = self;
    job->page       = page_to_submit;
    job->zoom       = self->zoom;
    job->rotation   = self->rotation;
    job->render_gen = self->render_gen;
    job->cancel_gen = self->cancel_gen;

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
   * ones render. Move current surfaces to prev_surface so they serve as
   * scaled placeholders during the re-render transition. */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->pages);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    CacheEntry *entry = value;
    entry->rendering = FALSE;  /* allow re-submission */
    /* Preserve the old surface as a zoom transition placeholder */
    if (entry->surface && entry->render_gen != self->render_gen) {
      if (entry->prev_surface)
        cairo_surface_destroy (entry->prev_surface);
      entry->prev_surface = entry->surface;
      entry->surface = NULL;
      FW_TRACE_MEM ("prev_surface stash: page=%d", GPOINTER_TO_INT (key));
    }
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
fw_cache_invalidate_all (FwCache *self)
{
  g_return_if_fail (FW_IS_CACHE (self));

  g_mutex_lock (&self->lock);
  guint old_surfaces = g_hash_table_size (self->pages);
  guint old_parsed   = g_hash_table_size (self->parsed);
  self->render_gen++;
  self->cancel_gen++;
  g_hash_table_remove_all (self->pages);

  /* Free all parsed page handles */
  if (self->document) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, self->parsed);
    while (g_hash_table_iter_next (&iter, &key, &value))
      parsed_entry_free_with_doc (value, self->document);
    g_hash_table_remove_all (self->parsed);
  }

  FW_TRACE_MEM ("invalidate_all: freed %u surfaces, %u parsed handles",
                old_surfaces, old_parsed);
  g_mutex_unlock (&self->lock);
}

void
fw_cache_invalidate_page (FwCache *self, int page)
{
  g_return_if_fail (FW_IS_CACHE (self));

  g_mutex_lock (&self->lock);
  g_hash_table_remove (self->pages, GINT_TO_POINTER (page));
  g_mutex_unlock (&self->lock);
}

void
fw_cache_set_priority (FwCache *self, const int *visible_pages, int n_visible)
{
  g_return_if_fail (FW_IS_CACHE (self));

  g_mutex_lock (&self->lock);

  /* Throttle: during cruising, skip rebuilds that arrive faster than
   * every 150 ms — the viewport is moving and most work becomes stale. */
  if (self->render_state == FW_RENDER_STATE_CRUISING) {
    gint64 now = g_get_monotonic_time ();
    if (now - self->last_priority_time < 150000) { /* 150 ms in µs */
      g_mutex_unlock (&self->lock);
      return;
    }
    self->last_priority_time = now;
  } else {
    self->last_priority_time = g_get_monotonic_time ();
  }

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

  /* Build priority: visible first, then a nearby buffer (7 forward,
   * 3 backward), then remaining pages outward.  This ensures the
   * immediate neighborhood is render-ready before filling the wider
   * cache window.
   *
   * During cruising, only populate the near buffer — the viewport is
   * moving and distant pages would be stale by the time they render.
   * The full 50-page window is populated once the user stops scrolling. */
  gboolean cruising = (self->render_state == FW_RENDER_STATE_CRUISING);
  int window = CACHE_WINDOW < total ? CACHE_WINDOW : total;
  self->priority_order = g_new (int, window);
  int idx = 0;

  #define NEAR_FORWARD 7
  #define NEAR_BACKWARD 3

  /* 1. Visible pages */
  for (int i = 0; i < n_visible && idx < window; i++)
    self->priority_order[idx++] = visible_pages[i];

  /* 2. Nearby forward buffer */
  int fwd_end = last_visible + 1 + NEAR_FORWARD;
  if (fwd_end > total) fwd_end = total;
  for (int i = last_visible + 1; i < fwd_end && idx < window; i++)
    self->priority_order[idx++] = i;

  /* 3. Nearby backward buffer */
  int bwd_start = first_visible - NEAR_BACKWARD;
  if (bwd_start < 0) bwd_start = 0;
  for (int i = first_visible - 1; i >= bwd_start && idx < window; i--)
    self->priority_order[idx++] = i;

  /* During cruising, stop here — don't fill the far window */
  if (cruising)
    goto done;

  /* 4. Remaining pages ahead (beyond the near buffer) */
  for (int i = fwd_end; i < total && idx < window; i++)
    self->priority_order[idx++] = i;

  /* 5. Remaining pages behind (beyond the near buffer) */
  for (int i = bwd_start - 1; i >= 0 && idx < window; i--)
    self->priority_order[idx++] = i;

done:
  self->priority_len = idx;

  /* Build keep-set from priority window */
  GHashTable *keep_set = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (int i = 0; i < self->priority_len; i++)
    g_hash_table_add (keep_set, GINT_TO_POINTER (self->priority_order[i]));

  /* ── Tier 1: Evict parsed page handles outside the priority window ──
   * Handles are created lazily by render workers — we only evict here.
   * IMPORTANT: skip pages with rendering=TRUE — a worker thread may be
   * holding the handle pointer between releasing self->lock and calling
   * render_page_from_handle. Evicting now would free it mid-use. */
  {
    GHashTableIter iter;
    gpointer key, value;
    GArray *to_remove = g_array_new (FALSE, FALSE, sizeof (int));
    g_hash_table_iter_init (&iter, self->parsed);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
      if (!g_hash_table_contains (keep_set, key)) {
        CacheEntry *ce = g_hash_table_lookup (self->pages, key);
        if (ce && ce->rendering)
          continue;  /* worker may be using this handle — skip */
        int pg = GPOINTER_TO_INT (key);
        g_array_append_val (to_remove, pg);
      }
    }
    for (guint i = 0; i < to_remove->len; i++) {
      int pg = g_array_index (to_remove, int, i);
      ParsedEntry *pe = g_hash_table_lookup (self->parsed, GINT_TO_POINTER (pg));
      if (pe) {
        parsed_entry_free_with_doc (pe, self->document);
        g_hash_table_steal (self->parsed, GINT_TO_POINTER (pg));
      }
    }
    g_array_unref (to_remove);
  }

  /* ── Tier 2: Evict rendered surfaces outside the window ── */
  {
    GHashTableIter iter;
    gpointer key, value;
    GArray *to_remove = g_array_new (FALSE, FALSE, sizeof (int));
    g_hash_table_iter_init (&iter, self->pages);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
      if (!g_hash_table_contains (keep_set, key)) {
        CacheEntry *entry = value;
        /* Skip pages with in-flight render jobs — the worker will store
         * its result soon.  Everything else outside the window is evicted
         * regardless of generation to bound memory usage. */
        if (!entry->rendering) {
          int pg = GPOINTER_TO_INT (key);
          g_array_append_val (to_remove, pg);
        }
      }
    }
    for (guint i = 0; i < to_remove->len; i++)
      g_hash_table_remove (self->pages,
                            GINT_TO_POINTER (g_array_index (to_remove, int, i)));
    g_array_unref (to_remove);
  }

  g_hash_table_unref (keep_set);

  FW_TRACE_MEM ("cache after eviction: surfaces=%u parsed=%u priority=%d",
                g_hash_table_size (self->pages),
                g_hash_table_size (self->parsed),
                self->priority_len);

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
  if (entry && entry->surface)
    surface = cairo_surface_reference (entry->surface);
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

cairo_surface_t *
fw_cache_get_prev_page (FwCache *self, int page)
{
  g_return_val_if_fail (FW_IS_CACHE (self), NULL);

  g_mutex_lock (&self->lock);
  CacheEntry *entry = g_hash_table_lookup (self->pages,
                                            GINT_TO_POINTER (page));
  cairo_surface_t *surface = NULL;
  if (entry && entry->prev_surface)
    surface = cairo_surface_reference (entry->prev_surface);
  g_mutex_unlock (&self->lock);

  return surface;
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

  FW_TRACE_CACHE ("stop");
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

  /* Wait for all pool threads to finish */
  if (self->pool) {
    g_thread_pool_free (self->pool, TRUE, TRUE);
    self->pool = NULL;
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

  /* Thread pool: use number of processors, non-exclusive so threads
   * are shared across pools. */
  int n_threads = (int) g_get_num_processors ();
  if (n_threads < 2) n_threads = 2;
  if (n_threads > 8) n_threads = 8;

  self->pool = g_thread_pool_new (render_worker, NULL, n_threads, FALSE, NULL);
  self->max_jobs = n_threads;
  self->active_jobs = 0;
  self->render_state = FW_RENDER_STATE_STATIC;
  self->scale_factor = 1;
}
