/* fw-document-cbr.c — Comic Book RAR (CBR) backend
 *
 * Uses libarchive (BSD, GPL-compatible) to enumerate RAR-archived images
 * and MuPDF for image decoding + zero-copy cairo paint. Page = one image
 * entry sorted by filename (universal comic-archive convention). The same
 * code path also handles ZIP-based comics if dispatched here, but the
 * factory routes those through the MuPDF backend's own CBZ handler.
 *
 * Threading: libarchive readers can't be shared across threads on the same
 * archive — every render call opens a fresh `archive *`, walks to the
 * target entry, extracts bytes, and closes. Multiple render threads can
 * each open their own reader, so the only serialization is access to the
 * shared `fz_context` for image decoding (one cairo surface gets created
 * per render; cheap on the cache miss path, never touched on cache hit).
 *
 * RAR sequential-stream cost: random page access requires walking entries
 * from the start. For a 200-page archive the worst case is ~5s of pure
 * RAR decompression. The velocity engine and thumbnail tier hide most of
 * this — the user reads while the cache pre-fetches forward in worker
 * threads. Sustained "scrub-to-end" remains slow on huge archives; that's
 * a fundamental property of streaming RAR. Future optimization: per-thread
 * persistent readers that remember their position.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document-cbr.h"
#include "fw-debug.h"

#include <gio/gio.h>
#include <archive.h>
#include <archive_entry.h>
#include <mupdf/fitz.h>
#include <string.h>

typedef struct {
  char    *name;       /* entry pathname inside the archive (owned) */
  gint64   size;       /* raw entry size in bytes */
} CbrEntry;

struct _FwDocumentCbr {
  GObject       parent_instance;

  char         *path;
  int           page_count;
  CbrEntry     *entries;          /* page_count items, sorted by name */

  double       *page_widths;
  double       *page_heights;

  fz_context   *ctx;              /* MuPDF context for image decoding */
  GMutex        ctx_lock;         /* serializes ctx access */
  GMutex        archive_lock;     /* serializes libarchive reads of self->path */

  volatile int  cancel_flag;      /* set during scrubbing aborts */

  /* Per-page bytes cache (Phase 14 follow-up). RAR has no central
   * directory, so seeking to entry N requires sequentially decompressing
   * entries 1..N-1 — every page render previously did this from scratch.
   * The cache stores already-extracted entry bytes keyed by page index,
   * so subsequent renders of the same page skip straight to MuPDF
   * decode. Bounded by `cache_byte_cap` (default 256 MB) with FIFO
   * eviction — comics are read mostly linearly, so age-based works
   * fine. Access serialized via `archive_lock` (same lock as the
   * archive walk it short-circuits). */
  GHashTable   *entry_cache;      /* int (page) → GBytes * (owned by table) */
  GQueue       *cache_order;      /* GINT_TO_POINTER (page) FIFO, head = oldest */
  gsize         cached_bytes;
  gsize         cache_byte_cap;

  /* Filename-based double-spread detection (v0.37, ported from
   * YACReader's `is_double_page` in `common/comic.cpp:925`). At open
   * time we walk the entry list, find the most common filename prefix
   * across basenames (path-stripped), and use it as the anchor for
   * detecting `<prefix><digits>.<ext>` spreads where the digit run
   * splits evenly into two consecutive page numbers (e.g.
   * `chapter01_034035.jpg`). `common_prefix == NULL` means we
   * couldn't agree on a prefix and the filename signal is disabled. */
  char         *common_prefix;
  int           max_spread_digits;

  /* Background dimension probe (v0.69). At open every page defaults to
   * the cover's size (see cbr_open); a one-shot worker decompresses the
   * archive once to learn real per-page dimensions, then posts them back
   * and emits "geometry-changed" so fit-width can normalize off the
   * typical body page instead of an oversized cover or spread. Guarded
   * so it runs at most once per document. */
  gboolean      dims_probed;
};

static void fw_document_cbr_iface_init (FwDocumentInterface *iface);
static char *cbr_compute_common_prefix (CbrEntry *entries, int page_count);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwDocumentCbr, fw_document_cbr, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE (FW_TYPE_DOCUMENT, fw_document_cbr_iface_init))

/* ── Helpers ──────────────────────────────────────────────────────── */

static gboolean
is_image_name (const char *name)
{
  if (!name || !name[0])
    return FALSE;
  /* Skip MacOS metadata directories (._foo and __MACOSX/) */
  const char *base = strrchr (name, '/');
  base = base ? base + 1 : name;
  if (g_str_has_prefix (base, "._"))
    return FALSE;
  if (g_str_has_prefix (name, "__MACOSX/") ||
      strstr (name, "/__MACOSX/"))
    return FALSE;

  const char *dot = strrchr (base, '.');
  if (!dot)
    return FALSE;
  return g_ascii_strcasecmp (dot, ".jpg")  == 0 ||
         g_ascii_strcasecmp (dot, ".jpeg") == 0 ||
         g_ascii_strcasecmp (dot, ".png")  == 0 ||
         g_ascii_strcasecmp (dot, ".gif")  == 0 ||
         g_ascii_strcasecmp (dot, ".webp") == 0 ||
         g_ascii_strcasecmp (dot, ".bmp")  == 0;
}

static int
entry_compare (gconstpointer a, gconstpointer b)
{
  const CbrEntry *ea = a;
  const CbrEntry *eb = b;
  return g_strcmp0 (ea->name, eb->name);
}

/* Open a fresh libarchive reader on self->path with RAR4+RAR5 support. */
static struct archive *
cbr_open_reader (FwDocumentCbr *self)
{
  struct archive *a = archive_read_new ();
  if (!a)
    return NULL;
  archive_read_support_format_rar  (a);
  archive_read_support_format_rar5 (a);
  /* CBT (tar) and CB7 (7z) — let one backend cover all archive comics
   * even though the factory currently routes them to the MuPDF path. */
  archive_read_support_format_tar  (a);
  archive_read_support_format_7zip (a);
  archive_read_support_format_zip  (a);

  if (archive_read_open_filename (a, self->path, 64 * 1024) != ARCHIVE_OK) {
    g_warning ("CBR: archive_read_open_filename: %s",
               archive_error_string (a));
    archive_read_free (a);
    return NULL;
  }
  return a;
}

/* Evict oldest cached entries (FIFO) until total cached bytes are at or
 * under cap. Caller must hold self->archive_lock. */
static void
cbr_cache_evict_to_cap (FwDocumentCbr *self)
{
  while (self->cached_bytes > self->cache_byte_cap &&
         !g_queue_is_empty (self->cache_order)) {
    gpointer head = g_queue_pop_head (self->cache_order);
    int pg = GPOINTER_TO_INT (head);
    GBytes *evicted = g_hash_table_lookup (self->entry_cache,
                                            GINT_TO_POINTER (pg));
    if (evicted) {
      self->cached_bytes -= g_bytes_get_size (evicted);
      g_hash_table_remove (self->entry_cache, GINT_TO_POINTER (pg));
    }
  }
}

/* Return the raw bytes of the archive entry for `page` as a refcounted
 * GBytes. Hit the per-page cache when possible; otherwise walk the
 * archive (the slow path that this cache exists to avoid). The returned
 * pointer is owned by the caller and must be unreffed with
 * g_bytes_unref. Returns NULL on error or cancellation.
 *
 * The cache is serialized via archive_lock (same lock as the archive
 * walk) so the slow-path walk and cache mutation never overlap. */
static GBytes *
cbr_extract_entry (FwDocumentCbr *self, int page)
{
  if (page < 0 || page >= self->page_count)
    return NULL;

  g_mutex_lock (&self->archive_lock);

  if (g_atomic_int_get (&self->cancel_flag)) {
    g_mutex_unlock (&self->archive_lock);
    return NULL;
  }

  /* Cache hit — bump the refcount and return immediately. The slow
   * libarchive walk is the entire reason this cache exists; a hit here
   * is the difference between a fan-spinning page flip and a quiet one. */
  GBytes *cached = g_hash_table_lookup (self->entry_cache,
                                         GINT_TO_POINTER (page));
  if (cached) {
    g_bytes_ref (cached);
    FW_TRACE_DOC ("cbr cache hit: page=%d bytes=%zu",
                  page, g_bytes_get_size (cached));
    g_mutex_unlock (&self->archive_lock);
    return cached;
  }
  FW_TRACE_DOC ("cbr cache miss: page=%d (walking archive)", page);

  /* Cache miss — walk the archive (RAR has no central directory, so this
   * decompresses every entry from 0..page sequentially) and extract. */
  struct archive *a = cbr_open_reader (self);
  if (!a) {
    g_mutex_unlock (&self->archive_lock);
    return NULL;
  }

  const char *target = self->entries[page].name;
  guint8 *bytes = NULL;
  size_t  bytes_size = 0;
  struct archive_entry *entry;

  while (archive_read_next_header (a, &entry) == ARCHIVE_OK) {
    if (g_atomic_int_get (&self->cancel_flag))
      break;

    const char *name = archive_entry_pathname (entry);
    if (g_strcmp0 (name, target) != 0) {
      archive_read_data_skip (a);
      continue;
    }

    gint64 sz = archive_entry_size (entry);
    /* Sanity cap per page: 256 MB. Larger than any real comic page. */
    if (sz <= 0 || sz > 256 * 1024 * 1024)
      break;

    bytes = g_malloc ((size_t) sz);
    /* archive_read_data may return less than requested; loop. */
    size_t total = 0;
    while (total < (size_t) sz) {
      la_ssize_t got = archive_read_data (a, bytes + total,
                                           (size_t) sz - total);
      if (got <= 0) {
        g_free (bytes);
        bytes = NULL;
        break;
      }
      total += (size_t) got;
    }
    if (bytes)
      bytes_size = (size_t) sz;
    break;
  }

  archive_read_free (a);

  GBytes *out = NULL;
  if (bytes) {
    /* g_bytes_new_take adopts the buffer; no extra copy. */
    out = g_bytes_new_take (bytes, bytes_size);
    /* Insert into cache: hash table holds one ref, caller's return holds
     * another. Push to FIFO order; evict oldest if over cap. */
    g_bytes_ref (out);
    g_hash_table_insert (self->entry_cache, GINT_TO_POINTER (page), out);
    g_queue_push_tail (self->cache_order, GINT_TO_POINTER (page));
    self->cached_bytes += bytes_size;
    cbr_cache_evict_to_cap (self);
  }

  g_mutex_unlock (&self->archive_lock);
  return out;
}

/* Render one image entry into a freshly allocated cairo ARGB32 surface,
 * applying zoom + rotation. Uses MuPDF's draw device wrapping cairo's
 * pixel buffer — the same zero-copy trick the PDF backend uses. */
static cairo_surface_t *
cbr_render (FwDocumentCbr *self, int page, double zoom, int rotation)
{
  GBytes *gbytes = cbr_extract_entry (self, page);
  if (!gbytes)
    return NULL;

  if (g_atomic_int_get (&self->cancel_flag)) {
    g_bytes_unref (gbytes);
    return NULL;
  }

  cairo_surface_t *surface = NULL;
  /* Set when the zero-size sub-pixel case fires below — benign
   * (zoom × image dims rounds to <1 px) so the catch handler
   * suppresses the warning. Genuine MuPDF errors still warn. */
  volatile gboolean silent_zero_size = FALSE;

  g_mutex_lock (&self->ctx_lock);

  fz_buffer  *buf       = NULL;
  fz_image   *img       = NULL;
  fz_pixmap  *cairo_pix = NULL;
  fz_device  *draw_dev  = NULL;

  size_t       sz    = 0;
  const guint8 *bytes = g_bytes_get_data (gbytes, &sz);

  fz_try (self->ctx) {
    buf = fz_new_buffer_from_copied_data (self->ctx, bytes, sz);
    img = fz_new_image_from_buffer (self->ctx, buf);

    int orig_w = img->w;
    int orig_h = img->h;
    if (orig_w < 1 || orig_h < 1)
      fz_throw (self->ctx, FZ_ERROR_GENERIC, "image has zero dimensions");

    /* Cache the real page size now that we know it (was a default until
     * the first render of each page). The view re-reads sizes via
     * get_page_size during recompute_layout — that won't fire mid-render,
     * but a subsequent zoom or rotation change recomputes layout, at
     * which point the actual dimensions take effect. */
    self->page_widths[page]  = (double) orig_w;
    self->page_heights[page] = (double) orig_h;

    /* Compute target pixel rectangle after zoom + rotation. */
    fz_matrix scale_rot = fz_scale ((float) zoom, (float) zoom);
    scale_rot = fz_concat (scale_rot, fz_rotate ((float) rotation));
    fz_rect bbox = fz_make_rect (0, 0, (float) orig_w, (float) orig_h);
    fz_rect transformed = fz_transform_rect (bbox, scale_rot);
    fz_irect pixel_rect = fz_round_rect (transformed);

    int w = pixel_rect.x1 - pixel_rect.x0;
    int h = pixel_rect.y1 - pixel_rect.y0;
    if (w < 1 || h < 1) {
      silent_zero_size = TRUE;
      fz_throw (self->ctx, FZ_ERROR_GENERIC, "zero-size render");
    }

    /* Allocate the destination cairo surface and clear it to white so
     * any narrower image doesn't leak garbage in the margins. */
    surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
      fz_throw (self->ctx, FZ_ERROR_GENERIC, "cairo_image_surface_create");
    cairo_surface_flush (surface);

    guint8 *data = cairo_image_surface_get_data (surface);

    /* For CAIRO_FORMAT_ARGB32, cairo's stride is always 4*w (pixel size
     * and alignment are both 4 bytes), which matches MuPDF's tight stride.
     * fz_new_pixmap_with_bbox_and_data uses that implicit stride. */
    cairo_pix = fz_new_pixmap_with_bbox_and_data (
      self->ctx, fz_device_bgr (self->ctx),
      fz_make_irect (0, 0, w, h), NULL, 1, data);
    fz_clear_pixmap_with_value (self->ctx, cairo_pix, 0xff);

    /* Image-to-pixmap matrix:
     *   (1) MuPDF images live in a (0..1) unit rect — scale to original
     *       pixel dimensions
     *   (2) apply zoom + rotation
     *   (3) translate so the rotated bbox starts at (0,0) of the pixmap
     */
    fz_matrix m = fz_scale ((float) orig_w, (float) orig_h);
    m = fz_concat (m, scale_rot);
    m = fz_concat (m, fz_translate ((float) -pixel_rect.x0,
                                     (float) -pixel_rect.y0));

    draw_dev = fz_new_draw_device (self->ctx, fz_identity, cairo_pix);
    fz_fill_image (self->ctx, draw_dev, img, m, 1.0,
                   fz_default_color_params);
    fz_close_device (self->ctx, draw_dev);

    cairo_surface_mark_dirty (surface);
  }
  fz_always (self->ctx) {
    if (draw_dev)  fz_drop_device  (self->ctx, draw_dev);
    if (cairo_pix) fz_drop_pixmap  (self->ctx, cairo_pix);
    if (img)       fz_drop_image   (self->ctx, img);
    if (buf)       fz_drop_buffer  (self->ctx, buf);
  }
  fz_catch (self->ctx) {
    if (!silent_zero_size)
      g_warning ("CBR: render failed page %d: %s",
                 page, fz_caught_message (self->ctx));
    if (surface) {
      cairo_surface_destroy (surface);
      surface = NULL;
    }
  }

  g_mutex_unlock (&self->ctx_lock);

  g_bytes_unref (gbytes);
  return surface;
}

/* ── FwDocumentInterface implementation ───────────────────────────── */

/* ── Background dimension probe ───────────────────────────────────── */

typedef struct {
  int     n;
  double *widths;
  double *heights;
} CbrDims;

static void
cbr_dims_free (CbrDims *d)
{
  if (!d)
    return;
  g_free (d->widths);
  g_free (d->heights);
  g_free (d);
}

/* Worker thread: one sequential pass over the archive, reading each
 * image's real dimensions via a MuPDF header decode. The reader is
 * independent (the file is read-only), so this runs alongside render
 * workers; ctx access is serialized on ctx_lock. Archive order need not
 * match page (filename-sorted) order, so dims are placed by name lookup.
 * Bails out on cancel_flag. */
static void
cbr_probe_dims_thread (GTask        *task,
                       gpointer      source,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  (void) task_data; (void) cancellable;
  FwDocumentCbr *self = source;

  CbrDims *d = g_new0 (CbrDims, 1);
  d->n       = self->page_count;
  d->widths  = g_new (double, self->page_count);
  d->heights = g_new (double, self->page_count);
  /* Seed with the open-time defaults so any entry we can't read keeps a
   * sane size. */
  for (int i = 0; i < self->page_count; i++) {
    d->widths[i]  = self->page_widths[i];
    d->heights[i] = self->page_heights[i];
  }

  /* name → page index, so an archive-order walk lands dims on the right
   * (sorted) page. */
  GHashTable *name_to_page = g_hash_table_new (g_str_hash, g_str_equal);
  for (int i = 0; i < self->page_count; i++)
    g_hash_table_insert (name_to_page, self->entries[i].name,
                         GINT_TO_POINTER (i + 1));

  struct archive *a = cbr_open_reader (self);
  if (a) {
    struct archive_entry *entry;
    while (archive_read_next_header (a, &entry) == ARCHIVE_OK) {
      if (g_atomic_int_get (&self->cancel_flag))
        break;
      const char *name = archive_entry_pathname (entry);
      gpointer slot = name ? g_hash_table_lookup (name_to_page, name) : NULL;
      if (!slot) {
        archive_read_data_skip (a);
        continue;
      }
      int page = GPOINTER_TO_INT (slot) - 1;
      gint64 sz = archive_entry_size (entry);
      if (sz <= 0 || sz > 256 * 1024 * 1024) {
        archive_read_data_skip (a);
        continue;
      }
      guint8 *bytes = g_malloc ((size_t) sz);
      size_t total = 0;
      gboolean ok = TRUE;
      while (total < (size_t) sz) {
        la_ssize_t got = archive_read_data (a, bytes + total,
                                            (size_t) sz - total);
        if (got <= 0) { ok = FALSE; break; }
        total += (size_t) got;
      }
      if (ok) {
        g_mutex_lock (&self->ctx_lock);
        fz_buffer *buf = NULL;
        fz_image  *img = NULL;
        fz_try (self->ctx) {
          buf = fz_new_buffer_from_copied_data (self->ctx, bytes, (size_t) sz);
          img = fz_new_image_from_buffer (self->ctx, buf);
          if (img->w > 0 && img->h > 0) {
            d->widths[page]  = (double) img->w;
            d->heights[page] = (double) img->h;
          }
        }
        fz_always (self->ctx) {
          if (img) fz_drop_image  (self->ctx, img);
          if (buf) fz_drop_buffer (self->ctx, buf);
        }
        fz_catch (self->ctx) {
          /* keep the seeded default for this page */
        }
        g_mutex_unlock (&self->ctx_lock);
      }
      g_free (bytes);
    }
    archive_read_free (a);
  }

  g_hash_table_destroy (name_to_page);
  g_task_return_pointer (task, d, (GDestroyNotify) cbr_dims_free);
}

/* Main thread: install the probed dimensions and, if any page changed
 * from its open-time default, tell the view to relayout + re-fit. The
 * write mirrors the render path's page_widths update, so it takes
 * ctx_lock to serialize against in-flight render writes. */
static void
cbr_probe_dims_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) user_data;
  FwDocumentCbr *self = FW_DOCUMENT_CBR (source);
  CbrDims *d = g_task_propagate_pointer (G_TASK (res), NULL);
  if (!d)
    return;

  gboolean cancelled = g_atomic_int_get (&self->cancel_flag);
  if (!cancelled && d->n == self->page_count) {
    g_mutex_lock (&self->ctx_lock);
    for (int i = 0; i < self->page_count; i++) {
      self->page_widths[i]  = d->widths[i];
      self->page_heights[i] = d->heights[i];
    }
    g_mutex_unlock (&self->ctx_lock);
  }
  cbr_dims_free (d);

  /* Always signal, not just when these differ from the open-time
   * defaults: the render path also writes page sizes from worker
   * threads, so by now self->page_widths may already match — but the
   * *view's* layout still holds the defaults and only refreshes on this
   * signal. The relayout is a no-op (invisible) when sizes match and
   * corrects the typical-page fit when they don't. */
  if (!cancelled) {
    FW_TRACE_DOC ("cbr probe done: page sizes finalized → geometry-changed");
    fw_document_emit_geometry_changed (FW_DOCUMENT (self));
  }
}

static void
cbr_start_dims_probe (FwDocumentCbr *self)
{
  if (self->dims_probed || self->page_count <= 1)
    return;
  self->dims_probed = TRUE;
  /* GTask refs `self` for the task's lifetime, so dispose (and the
   * cbr_close that frees entries / path) can't run until the probe
   * completes — the worker's reads of those fields stay valid. */
  GTask *task = g_task_new (self, NULL, cbr_probe_dims_done, NULL);
  g_task_run_in_thread (task, cbr_probe_dims_thread);
  g_object_unref (task);
}

static gboolean
cbr_open (FwDocument *doc, const char *path, GError **error)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (doc);

  self->ctx = fz_new_context (NULL, NULL, 64 << 20);
  if (!self->ctx) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Failed to create MuPDF context for image decoding");
    return FALSE;
  }
  fz_register_document_handlers (self->ctx);

  self->path = g_strdup (path);

  /* Phase 1: enumerate image entries. */
  struct archive *a = cbr_open_reader (self);
  if (!a) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
      "Cannot open archive (libarchive may lack RAR support — "
      "check that libarchive was built with --enable-rar)");
    fz_drop_context (self->ctx);
    self->ctx = NULL;
    g_clear_pointer (&self->path, g_free);
    return FALSE;
  }

  GArray *entries = g_array_new (FALSE, FALSE, sizeof (CbrEntry));
  struct archive_entry *entry;
  while (archive_read_next_header (a, &entry) == ARCHIVE_OK) {
    const char *name = archive_entry_pathname (entry);
    if (!is_image_name (name)) {
      archive_read_data_skip (a);
      continue;
    }
    CbrEntry e = {
      .name = g_strdup (name),
      .size = archive_entry_size (entry),
    };
    g_array_append_val (entries, e);
    archive_read_data_skip (a);
  }
  archive_read_free (a);

  if (entries->len == 0) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Comic archive contains no image files");
    g_array_unref (entries);
    fz_drop_context (self->ctx);
    self->ctx = NULL;
    g_clear_pointer (&self->path, g_free);
    return FALSE;
  }

  self->page_count = (int) entries->len;
  g_array_sort (entries, entry_compare);
  self->entries = (CbrEntry *) g_array_free (entries, FALSE);

  /* Compute the filename-prefix anchor for double-spread detection.
   * Cheap (O(page_count) + a small hash), runs once at open. NULL
   * result means we couldn't agree on a prefix and the filename
   * signal will be inert — aspect-ratio detection still applies. */
  self->common_prefix = cbr_compute_common_prefix (self->entries,
                                                    self->page_count);
  /* YACReader's heuristic for max digit-run length: 2× the number of
   * digits in the page count. e.g. 100 pages → 6 digits ("034035"). */
  self->max_spread_digits = 0;
  {
    int n = self->page_count;
    while (n > 0) { self->max_spread_digits++; n /= 10; }
    self->max_spread_digits *= 2;
  }

  self->page_widths  = g_new (double, self->page_count);
  self->page_heights = g_new (double, self->page_count);

  /* Phase 2: peek at page 0 for representative dimensions; use as the
   * default for every page. Each page's actual dims override this when
   * it first renders. For 99% of comics every page is the same size, so
   * the default is correct from the start. The bytes-cache populated
   * here means the first real render of page 0 is also instant. */
  double default_w = 1280, default_h = 1920;
  GBytes *first_gbytes = cbr_extract_entry (self, 0);
  if (first_gbytes) {
    size_t first_sz = 0;
    const guint8 *first_bytes = g_bytes_get_data (first_gbytes, &first_sz);
    g_mutex_lock (&self->ctx_lock);
    fz_buffer *buf = NULL;
    fz_image  *img = NULL;
    fz_try (self->ctx) {
      buf = fz_new_buffer_from_copied_data (self->ctx, first_bytes, first_sz);
      img = fz_new_image_from_buffer (self->ctx, buf);
      default_w = (double) img->w;
      default_h = (double) img->h;
    }
    fz_always (self->ctx) {
      if (img) fz_drop_image  (self->ctx, img);
      if (buf) fz_drop_buffer (self->ctx, buf);
    }
    fz_catch (self->ctx) {
      g_warning ("CBR: failed to probe page 0 dimensions: %s",
                 fz_caught_message (self->ctx));
    }
    g_mutex_unlock (&self->ctx_lock);
    g_bytes_unref (first_gbytes);
  }

  for (int i = 0; i < self->page_count; i++) {
    self->page_widths[i]  = default_w;
    self->page_heights[i] = default_h;
  }

  FW_TRACE_DOC ("cbr open: '%s' %d pages, default size %.0fx%.0f",
                path, self->page_count, default_w, default_h);

  /* Learn real per-page dimensions in the background so fit-width can
   * normalize off the typical page (the open-time default above assumes
   * every page matches the cover). */
  cbr_start_dims_probe (self);

  return TRUE;
}

static void
cbr_close (FwDocument *doc)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (doc);

  /* Stop any in-flight background dimension probe before tearing down
   * the entries / path it reads. The GTask holds a ref on self, so
   * dispose (hence this close) can't actually run mid-probe — but on a
   * scrubbing-triggered close the flag lets the worker bail early
   * instead of finishing a pointless full decompress. */
  g_atomic_int_set (&self->cancel_flag, 1);

  /* Drop the per-page bytes cache before any of the surrounding state.
   * GBytes destroy via the hash's GDestroyNotify. */
  if (self->entry_cache) {
    g_hash_table_remove_all (self->entry_cache);
  }
  if (self->cache_order) {
    g_queue_clear (self->cache_order);
  }
  self->cached_bytes = 0;

  if (self->entries) {
    for (int i = 0; i < self->page_count; i++)
      g_free (self->entries[i].name);
    g_free (self->entries);
    self->entries = NULL;
  }
  g_clear_pointer (&self->page_widths,  g_free);
  g_clear_pointer (&self->page_heights, g_free);
  g_clear_pointer (&self->path,         g_free);
  g_clear_pointer (&self->common_prefix, g_free);
  if (self->ctx) {
    fz_drop_context (self->ctx);
    self->ctx = NULL;
  }
  self->page_count = 0;
}

static int
cbr_get_page_count (FwDocument *doc)
{
  return FW_DOCUMENT_CBR (doc)->page_count;
}

static void
cbr_get_page_size (FwDocument *doc, int page, double *width, double *height)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (doc);
  if (page < 0 || page >= self->page_count) {
    if (width)  *width  = 0;
    if (height) *height = 0;
    return;
  }
  if (width)  *width  = self->page_widths[page];
  if (height) *height = self->page_heights[page];
}

static cairo_surface_t *
cbr_render_page (FwDocument *doc, int page, double zoom, int rotation)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (doc);
  return cbr_render (self, page, zoom, rotation);
}

static FwTocNode *
cbr_get_toc (FwDocument *doc)
{
  /* Comic archives don't carry a TOC. The empty sidebar placeholder
   * shows "No table of contents" — appropriate for this format. */
  (void) doc;
  return NULL;
}

static GArray *
cbr_search (FwDocument *doc, const char *text, int page)
{
  /* CBR pages are images — no text layer. Return an empty array so the
   * search controller's progress beat continues on through the document
   * without spurious empty hits. */
  (void) doc; (void) text; (void) page;
  return g_array_new (FALSE, FALSE, sizeof (FwSearchHit));
}

/* ── Filename-based double-spread detection (Phase 13 follow-up) ──
 *
 * Direct port of YACReader's algorithm in `common/comic.cpp:925-1028`.
 * Algorithm: walk the (sorted) entry list, find the most common
 * prefix among adjacent basenames. If frequency ≥ 60% of pages and
 * the prefix isn't all-digits, it's the anchor. A page is a spread
 * when its name minus prefix yields an even-length all-digit run
 * whose two halves parse as consecutive page numbers (right-left=1).
 *
 * Complementary to the v0.27.1 aspect-ratio test — catches
 * scanlation rips where the spread image is the same dimensions as
 * a single page (so the aspect ratio looks normal) but the filename
 * encodes both numbers like `chapter01_034035.jpg`. */

static const char *
cbr_basename (const char *path)
{
  const char *slash = strrchr (path, '/');
  return slash ? slash + 1 : path;
}

static char *
cbr_compute_common_prefix (CbrEntry *entries, int page_count)
{
  if (page_count < 2)
    return NULL;

  /* Frequency table keyed by the candidate prefix string. As we walk
   * the (sorted) basenames pairwise, we shrink current_len whenever
   * an adjacent pair diverges sooner; we record a run whenever the
   * prefix changes. The final winner is the prefix with highest
   * frequency, provided it's neither all-digits nor too rare. */
  GHashTable *frequency =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  const char *prev = cbr_basename (entries[0].name);
  int current_len = (int) strlen (prev);
  int current_count = 1;

  for (int i = 1; i < page_count; i++) {
    const char *cur = cbr_basename (entries[i].name);
    int pos = 0;
    while (cur[pos] && prev[pos] && cur[pos] == prev[pos])
      pos++;

    if (pos < current_len && pos > 0) {
      char *key = g_strndup (prev, current_len);
      gpointer existing = g_hash_table_lookup (frequency, key);
      g_hash_table_replace (frequency, key,
        GUINT_TO_POINTER (GPOINTER_TO_UINT (existing) + (guint) current_count));
      current_len = pos;
      current_count++;
    } else if (pos == 0) {
      char *key = g_strndup (prev, current_len);
      gpointer existing = g_hash_table_lookup (frequency, key);
      g_hash_table_replace (frequency, key,
        GUINT_TO_POINTER (GPOINTER_TO_UINT (existing) + (guint) current_count));
      current_len = (int) strlen (cur);
      current_count = 1;
    } else {
      current_count++;
    }
    prev = cur;
  }

  /* Final run. */
  {
    char *key = g_strndup (prev, current_len);
    gpointer existing = g_hash_table_lookup (frequency, key);
    g_hash_table_replace (frequency, key,
      GUINT_TO_POINTER (GPOINTER_TO_UINT (existing) + (guint) current_count));
  }

  guint max_freq = 0;
  const char *winner = NULL;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, frequency);
  while (g_hash_table_iter_next (&it, &k, &v)) {
    if (GPOINTER_TO_UINT (v) > max_freq) {
      max_freq = GPOINTER_TO_UINT (v);
      winner = (const char *) k;
    }
  }

  char *result = NULL;
  if (winner && max_freq >= (guint) (page_count * 0.6)) {
    /* Reject all-digit prefixes — those are usually the page-number
     * field itself, not a structural prefix. */
    gboolean all_digits = TRUE;
    for (const char *p = winner; *p; p++) {
      if (!g_ascii_isdigit (*p)) { all_digits = FALSE; break; }
    }
    if (!all_digits)
      result = g_strdup (winner);
  }

  g_hash_table_destroy (frequency);
  return result;
}

static gboolean
cbr_filename_is_spread (const char *page_name,
                        const char *common_prefix,
                        int max_digits)
{
  if (!common_prefix || !page_name)
    return FALSE;

  const char *base = cbr_basename (page_name);
  size_t plen = strlen (common_prefix);
  if (strncmp (base, common_prefix, plen) != 0)
    return FALSE;

  const char *digits = base + plen;
  int n = 0;
  while (digits[n] && g_ascii_isdigit (digits[n]) && n < 16)
    n++;

  if (n < 4 || n > max_digits || (n % 2) != 0)
    return FALSE;

  /* Split in half, parse as ints, require right - left == 1. */
  char left[9], right[9];
  int half = n / 2;
  if (half >= (int) sizeof (left)) return FALSE;
  memcpy (left,  digits,        half); left[half]  = '\0';
  memcpy (right, digits + half, half); right[half] = '\0';
  int li = atoi (left);
  int ri = atoi (right);
  return li > 0 && ri > 0 && (ri - li) == 1;
}

static gboolean
cbr_is_spread_filename (FwDocument *doc, int page)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (doc);
  if (page < 0 || page >= self->page_count)
    return FALSE;
  return cbr_filename_is_spread (self->entries[page].name,
                                  self->common_prefix,
                                  self->max_spread_digits);
}

static char *
cbr_get_text (FwDocument *doc, int page,
              double x0, double y0, double x1, double y1)
{
  (void) doc; (void) page; (void) x0; (void) y0; (void) x1; (void) y1;
  return NULL;
}

static GArray *
cbr_get_links (FwDocument *doc, int page)
{
  (void) doc; (void) page;
  return NULL;
}

/* The page-handle API exists so the cache can split parsing (I/O) from
 * rendering (CPU). For CBR the "parse" step would be the libarchive
 * walk-and-extract — which is exactly what render_page already does. We
 * stub these as identity passthroughs so the cache's parsed-window code
 * still works; the cost saving comes through naturally on cache-hit
 * lookups (no re-extract). Future optimization: cache extracted bytes
 * inside the parsed-window so render_page_from_handle can decode without
 * re-walking the archive. */
static gpointer
cbr_open_page (FwDocument *doc, int page)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (doc);
  if (page < 0 || page >= self->page_count)
    return NULL;
  /* Non-NULL sentinel — the cache only checks for NULL to mean "open
   * failed". Storing page+1 lets us recover the index in
   * render_page_from_handle. */
  return GINT_TO_POINTER (page + 1);
}

static void
cbr_close_page (FwDocument *doc, gpointer handle)
{
  (void) doc; (void) handle;
}

static cairo_surface_t *
cbr_render_page_from_handle (FwDocument *doc, gpointer handle,
                             double zoom, int rotation)
{
  int page = GPOINTER_TO_INT (handle) - 1;
  return cbr_render_page (doc, page, zoom, rotation);
}

static void
cbr_cancel_render (FwDocument *doc)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (doc);
  g_atomic_int_set (&self->cancel_flag, 1);
}

static void
fw_document_cbr_iface_init (FwDocumentInterface *iface)
{
  iface->open                    = cbr_open;
  iface->close                   = cbr_close;
  iface->get_page_count          = cbr_get_page_count;
  iface->get_page_size           = cbr_get_page_size;
  iface->render_page             = cbr_render_page;
  iface->get_toc                 = cbr_get_toc;
  iface->search                  = cbr_search;
  iface->get_text                = cbr_get_text;
  iface->get_links               = cbr_get_links;
  iface->open_page               = cbr_open_page;
  iface->close_page              = cbr_close_page;
  iface->render_page_from_handle = cbr_render_page_from_handle;
  iface->cancel_render           = cbr_cancel_render;
  iface->is_spread_filename      = cbr_is_spread_filename;
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_document_cbr_dispose (GObject *object)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (object);
  cbr_close (FW_DOCUMENT (self));
  G_OBJECT_CLASS (fw_document_cbr_parent_class)->dispose (object);
}

static void
fw_document_cbr_finalize (GObject *object)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (object);
  g_mutex_clear (&self->ctx_lock);
  g_mutex_clear (&self->archive_lock);
  g_clear_pointer (&self->entry_cache, g_hash_table_unref);
  if (self->cache_order) {
    g_queue_free (self->cache_order);
    self->cache_order = NULL;
  }
  G_OBJECT_CLASS (fw_document_cbr_parent_class)->finalize (object);
}

static void
fw_document_cbr_class_init (FwDocumentCbrClass *klass)
{
  GObjectClass *o = G_OBJECT_CLASS (klass);
  o->dispose  = fw_document_cbr_dispose;
  o->finalize = fw_document_cbr_finalize;
}

static void
fw_document_cbr_init (FwDocumentCbr *self)
{
  g_mutex_init (&self->ctx_lock);
  g_mutex_init (&self->archive_lock);
  /* GBytes destroy handled by GHashTable via g_bytes_unref. */
  self->entry_cache = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                              NULL, (GDestroyNotify) g_bytes_unref);
  self->cache_order = g_queue_new ();
  self->cached_bytes = 0;
  self->cache_byte_cap = 256u * 1024u * 1024u;  /* 256 MB */
}

FwDocumentCbr *
fw_document_cbr_new (void)
{
  return g_object_new (FW_TYPE_DOCUMENT_CBR, NULL);
}
