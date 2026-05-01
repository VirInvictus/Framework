/* fw-document-pdf.c — MuPDF backend implementation
 *
 * MuPDF uses setjmp/longjmp for exception handling via fz_try/fz_catch.
 * CRITICAL RULES:
 *   - NEVER return/goto/longjmp from inside fz_try or fz_catch blocks
 *   - Variables modified in fz_try and read in fz_catch must be volatile
 *   - Use fz_always for cleanup of resources allocated inside fz_try
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document-pdf.h"
#include "fw-debug.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdint.h>
#include <string.h>

#define MAX_RENDER_INSTANCES 8

/* ── Parallel rendering via independent document instances ────────── */
/* MuPDF's fz_context and fz_document are NOT thread-safe. Cloned contexts
 * share a font/image store, but fz_page and fz_image objects lazily read
 * from PDF streams that are owned by the document — concurrent reads from
 * multiple threads corrupt state (even through display lists).
 *
 * The correct multi-threading approach: open the same file N times, each
 * with its own fz_context and fz_document. Each render thread picks an
 * instance and works completely independently — no shared state at all. */

typedef struct {
  fz_context  *ctx;
  fz_document *doc;
  GMutex       lock;
} RenderInstance;

struct _FwDocumentPdf {
  GObject       parent_instance;

  fz_context   *ctx;          /* main context — used for page loading, TOC, search */
  fz_document  *doc;
  char         *path;
  int           page_count;
  GMutex        lock;         /* serializes main context access (page load, TOC, etc.) */

  /* Independent render instances — each opens the file separately so there
   * is zero shared state between render threads */
  RenderInstance render[MAX_RENDER_INSTANCES];
  int            n_render;
  volatile int   next_render;  /* round-robin index */

  /* Page sizes cached at open time to avoid repeated fz_load_page calls */
  double       *page_widths;   /* in points */
  double       *page_heights;

  /* Phase 11 Tier 1: in-flight render cancellation via fz_cookie.
   * Each active render publishes its cookie pointer here under
   * cookies_lock. pdf_cancel_render iterates and writes cookie->abort = 1
   * on every active render — MuPDF sees the flag during its next
   * fz_run_page checkpoint and returns immediately, abandoning whatever
   * partial output it has produced. cookies_lock is a separate mutex
   * from the per-instance render lock so cancel never blocks on the
   * worker that's mid-render. Pointers live on the worker's stack and
   * are deregistered before fz_run_page's caller returns. */
  GMutex        cookies_lock;
  fz_cookie    *active_cookies[MAX_RENDER_INSTANCES];

  /* Phase 11 Tier 1: cached structured-text per page (`fz_stext_page`).
   * Populated lazily on first text-related call (selection, search) for
   * a given page; reused on every subsequent call. Eliminates the
   * fz_load_page + fz_new_stext_page_from_page round-trip that used to
   * happen for every get_text and every search-on-this-page. Owned by
   * `self->ctx`, dropped en masse in pdf_close. Access is serialized
   * via `self->lock` (same as the rest of main-ctx state). */
  GHashTable   *stext_cache;   /* int → fz_stext_page* */
};

static void fw_document_pdf_iface_init (FwDocumentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwDocumentPdf, fw_document_pdf, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE (FW_TYPE_DOCUMENT, fw_document_pdf_iface_init))

/* ── Helpers ──────────────────────────────────────────────────────── */

static fz_matrix
build_transform (double zoom, int rotation)
{
  fz_matrix m = fz_scale ((float) zoom, (float) zoom);
  m = fz_concat (m, fz_rotate ((float) rotation));
  return m;
}

/* Render a page directly into a cairo ARGB32 surface with zero pixel copying.
 *
 * MuPDF's BGR colorspace + alpha=1 produces 32-bit pixels in B,G,R,A byte
 * order — which on little-endian systems is exactly cairo's ARGB32 layout.
 * By passing the cairo surface's own pixel buffer as the pixmap backing,
 * MuPDF's draw device writes rendered pixels straight into the final buffer
 * with no intermediate allocation, no channel shuffle, no alpha premultiply
 * loop.  This is the same technique zathura-pdf-mupdf uses.
 *
 * For ARGB32, `cairo_format_stride_for_width` returns `4*w` (always 4-byte
 * aligned for any width), so cairo's stride and MuPDF's tight stride match.
 *
 * The `cookie` argument is plumbed to fz_run_page so that
 * pdf_cancel_render can flip cookie->abort = 1 from another thread and
 * MuPDF will abandon the in-flight render at its next checkpoint
 * instead of running to completion. NULL cookie = no in-flight cancel.
 *
 * Called with inst->lock held. */
static cairo_surface_t *
render_page_direct (fz_context *ctx, fz_page *page,
                    double zoom, int rotation, fz_cookie *cookie)
{
  fz_rect bbox = fz_bound_page (ctx, page);
  fz_matrix m = build_transform (zoom, rotation);
  fz_rect transformed = fz_transform_rect (bbox, m);
  fz_irect pixel_rect = fz_round_rect (transformed);

  int w = pixel_rect.x1 - pixel_rect.x0;
  int h = pixel_rect.y1 - pixel_rect.y0;
  if (w < 1 || h < 1)
    return NULL;

  cairo_surface_t *surface =
    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy (surface);
    return NULL;
  }

  cairo_surface_flush (surface);
  unsigned char *samples = cairo_image_surface_get_data (surface);

  /* Translate so the rendered page lands at (0,0) in the pixel buffer. */
  fz_matrix draw_m = fz_concat (m,
    fz_translate (-transformed.x0, -transformed.y0));

  fz_irect pix_bbox = { .x0 = 0, .y0 = 0, .x1 = w, .y1 = h };
  volatile fz_pixmap *pix = NULL;
  volatile fz_device *dev = NULL;

  fz_try (ctx) {
    /* Build a pixmap wrapping the cairo surface buffer. BGR + alpha=1 gives
     * B,G,R,A bytes = cairo ARGB32 on little-endian. */
    pix = fz_new_pixmap_with_bbox_and_data (ctx,
            fz_device_bgr (ctx),
            pix_bbox,
            NULL, 1, samples);
    /* Fill with opaque white before rendering — matches zathura's behaviour
     * and avoids transparent edges on pages that don't cover the full bbox. */
    fz_clear_pixmap_with_value (ctx, (fz_pixmap *) pix, 0xFF);

    dev = fz_new_draw_device (ctx, fz_identity, (fz_pixmap *) pix);
    fz_run_page (ctx, page, (fz_device *) dev, draw_m, cookie);
    fz_close_device (ctx, (fz_device *) dev);
  }
  fz_always (ctx) {
    fz_drop_device (ctx, (fz_device *) dev);
    /* Drop the pixmap — it doesn't own the samples buffer (we passed it in)
     * so this just frees the pixmap struct, not the cairo buffer. */
    fz_drop_pixmap (ctx, (fz_pixmap *) pix);
  }
  fz_catch (ctx) {
    g_warning ("MuPDF: render failed: %s", fz_caught_message (ctx));
    cairo_surface_destroy (surface);
    return NULL;
  }

  /* Mid-render abort: cookie->abort got flipped to 1 from another thread
   * during fz_run_page. The surface contains whatever pixels MuPDF
   * managed to draw before bailing — discard. The 1-3 s saved per
   * abandoned big-PDF page is the whole point of plumbing the cookie. */
  if (cookie && cookie->abort) {
    cairo_surface_destroy (surface);
    return NULL;
  }

  cairo_surface_mark_dirty (surface);
  return surface;
}

/* ── Custom warning handler — route through GLib ─────────────────── */

static void
pdf_warn_handler (void *user, const char *message)
{
  (void) user;
  /* Suppress noisy freetype cmap warnings; log everything else */
  if (strstr (message, "freetype could not find") == NULL)
    g_debug ("MuPDF: %s", message);
}

static void
pdf_error_handler (void *user, const char *message)
{
  (void) user;
  g_warning ("MuPDF: %s", message);
}

/* Pick a MuPDF store size from the document's file size. The store
 * caches decoded fonts and images within MuPDF; bigger means more cache
 * hits during render at the cost of resident memory.
 *
 * Previously hardcoded to 64 MB × (1 main + 8 render instances) = 576 MB
 * worst case for stores alone, regardless of document size. A 200-page
 * novel EPUB doesn't need that much; a 50 MB scanned textbook benefits
 * from more. This function reads the file size once at open time and
 * picks a scaled value applied to every context. */
static size_t
pdf_pick_store_size (const char *path)
{
  GStatBuf st;
  if (!path || g_stat (path, &st) != 0)
    return 32u << 20;  /* 32 MB conservative fallback */

  gint64 sz = (gint64) st.st_size;
  if (sz < (gint64) (5  << 20)) return 16u << 20;   /* small EPUB/MOBI */
  if (sz < (gint64) (20 << 20)) return 32u << 20;   /* most novels, technical PDFs under ~500 pages */
  if (sz < (gint64) (100 << 20)) return 64u << 20;  /* typical large textbook */
  return 128u << 20;                                /* big scanned book / huge XPS */
}

/* ── Interface implementation ─────────────────────────────────────── */

static gboolean
pdf_open (FwDocument *doc, const char *path, GError **error)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);

  size_t store_size = pdf_pick_store_size (path);
  FW_TRACE_PDF ("store size: %zu MB for '%s'", store_size >> 20, path);

  fz_context *ctx = fz_new_context (NULL, NULL, store_size);
  if (!ctx) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Failed to create MuPDF context");
    return FALSE;
  }

  fz_set_warning_callback (ctx, pdf_warn_handler, NULL);
  fz_set_error_callback (ctx, pdf_error_handler, NULL);

  volatile int failed = 0;
  const char *errmsg = NULL;

  fz_try (ctx) {
    fz_register_document_handlers (ctx);
    self->doc = fz_open_document (ctx, path);
    /* Reflowable formats (EPUB, FB2, MOBI) need a layout pass before page
     * count is meaningful. Fixed-layout formats (PDF, CBZ, XPS) return
     * FALSE and skip the call. 600×900 pt at 11pt is a sensible default
     * page size for novel-shaped reading; pages stay this size for the
     * document's lifetime in this session. */
    if (fz_is_document_reflowable (ctx, self->doc))
      fz_layout_document (ctx, self->doc, 600, 900, 11);
    self->page_count = fz_count_pages (ctx, self->doc);
  }
  fz_catch (ctx) {
    errmsg = fz_caught_message (ctx);
    failed = 1;
  }

  if (failed) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "MuPDF: %s", errmsg ? errmsg : "unknown error");
    if (self->doc) {
      fz_drop_document (ctx, self->doc);
      self->doc = NULL;
    }
    fz_drop_context (ctx);
    return FALSE;
  }

  self->ctx  = ctx;
  self->path = g_strdup (path);
  FW_TRACE_PDF ("opened '%s': %d pages", path, self->page_count);

  /* Create independent render instances — each opens the file separately
   * with its own fz_context and fz_document. No shared state between
   * render threads means no synchronization issues, even for PDFs with
   * JPEG2000 images or complex color spaces. */
  int n_threads = (int) g_get_num_processors ();
  if (n_threads < 2) n_threads = 2;
  if (n_threads > MAX_RENDER_INSTANCES) n_threads = MAX_RENDER_INSTANCES;
  self->n_render = n_threads;
  self->next_render = 0;

  for (int i = 0; i < self->n_render; i++) {
    self->render[i].ctx = fz_new_context (NULL, NULL, store_size);
    if (self->render[i].ctx) {
      fz_set_warning_callback (self->render[i].ctx, pdf_warn_handler, NULL);
      fz_set_error_callback (self->render[i].ctx, pdf_error_handler, NULL);
      fz_register_document_handlers (self->render[i].ctx);
      fz_try (self->render[i].ctx) {
        self->render[i].doc = fz_open_document (self->render[i].ctx, path);
        /* Reflowable formats need an identical layout pass per instance,
         * otherwise different render threads see different page bounds. */
        if (fz_is_document_reflowable (self->render[i].ctx,
                                        self->render[i].doc))
          fz_layout_document (self->render[i].ctx, self->render[i].doc,
                              600, 900, 11);
      }
      fz_catch (self->render[i].ctx) {
        g_warning ("MuPDF: render instance %d failed to open: %s",
                   i, fz_caught_message (self->render[i].ctx));
        fz_drop_context (self->render[i].ctx);
        self->render[i].ctx = NULL;
        self->render[i].doc = NULL;
      }
    }
    g_mutex_init (&self->render[i].lock);
  }

  /* Pre-cache all page sizes using the fast PDF-specific API.
   * pdf_page_obj_transform reads MediaBox from the page dict
   * without loading the full page — orders of magnitude faster. */
  self->page_widths  = g_new (double, self->page_count);
  self->page_heights = g_new (double, self->page_count);

  pdf_document *pdoc = pdf_specifics (ctx, self->doc);

  for (int i = 0; i < self->page_count; i++) {
    self->page_widths[i]  = 612.0;
    self->page_heights[i] = 792.0;

    fz_try (ctx) {
      if (pdoc) {
        /* Fast path: read MediaBox directly from page dict */
        pdf_obj *pageobj = pdf_lookup_page_obj (ctx, pdoc, i);
        fz_rect mediabox;
        fz_matrix ctm;
        pdf_page_obj_transform (ctx, pageobj, &mediabox, &ctm);
        self->page_widths[i]  = (double) (mediabox.x1 - mediabox.x0);
        self->page_heights[i] = (double) (mediabox.y1 - mediabox.y0);
      } else {
        /* Fallback for non-PDF (shouldn't hit this path) */
        fz_page *pg = fz_load_page (ctx, self->doc, i);
        fz_rect bounds = fz_bound_page (ctx, pg);
        self->page_widths[i]  = (double) (bounds.x1 - bounds.x0);
        self->page_heights[i] = (double) (bounds.y1 - bounds.y0);
        fz_drop_page (ctx, pg);
      }
    }
    fz_catch (ctx) {
      /* keep fallback values */
    }
  }

  return TRUE;
}

static void
pdf_close (FwDocument *doc)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  FW_TRACE_PDF ("close: '%s'", self->path ? self->path : "(null)");

  /* Drop cached structured-text pages first — they're owned by
   * self->ctx, which is dropped further down. After this loop the
   * hash table is empty but still allocated; finalize unrefs it. */
  if (self->stext_cache && self->ctx) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, self->stext_cache);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
      fz_drop_stext_page (self->ctx, (fz_stext_page *) value);
    }
    g_hash_table_remove_all (self->stext_cache);
  }

  /* Drop independent render instances first */
  for (int i = 0; i < self->n_render; i++) {
    if (self->render[i].doc) {
      fz_drop_document (self->render[i].ctx, self->render[i].doc);
      self->render[i].doc = NULL;
    }
    if (self->render[i].ctx) {
      fz_drop_context (self->render[i].ctx);
      self->render[i].ctx = NULL;
    }
    g_mutex_clear (&self->render[i].lock);
  }
  self->n_render = 0;

  if (self->doc) {
    fz_drop_document (self->ctx, self->doc);
    self->doc = NULL;
  }
  if (self->ctx) {
    fz_drop_context (self->ctx);
    self->ctx = NULL;
  }
  g_clear_pointer (&self->path, g_free);
  g_clear_pointer (&self->page_widths, g_free);
  g_clear_pointer (&self->page_heights, g_free);
  self->page_count = 0;
}

static int
pdf_get_page_count (FwDocument *doc)
{
  return FW_DOCUMENT_PDF (doc)->page_count;
}

static void
pdf_get_page_size (FwDocument *doc, int page,
                   double *width, double *height)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);

  if (page >= 0 && page < self->page_count && self->page_widths) {
    if (width)  *width  = self->page_widths[page];
    if (height) *height = self->page_heights[page];
  } else {
    if (width)  *width  = 612.0;
    if (height) *height = 792.0;
  }
}

static cairo_surface_t *
pdf_render_page (FwDocument *doc, int page, double zoom, int rotation)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  cairo_surface_t *surface = NULL;
  FW_TRACE_PDF ("render_page start: page=%d zoom=%.2f rot=%d", page, zoom, rotation);

  /* Grab an independent render instance via round-robin.
   * Each instance has its own context + document — fully thread-safe.
   * Cast to unsigned before modulo to avoid negative indices on int overflow. */
  int slot = (int) ((unsigned int) g_atomic_int_add (&self->next_render, 1) % (unsigned int) self->n_render);
  RenderInstance *inst = &self->render[slot];

  fz_context  *ctx = (inst->ctx && inst->doc) ? inst->ctx : self->ctx;
  fz_document *pdf = (inst->ctx && inst->doc) ? inst->doc : self->doc;
  GMutex      *lock = (inst->ctx && inst->doc) ? &inst->lock : &self->lock;

  g_mutex_lock (lock);

  volatile fz_page *pg = NULL;
  fz_try (ctx) {
    pg = fz_load_page (ctx, pdf, page);
  }
  fz_catch (ctx) {
    g_warning ("MuPDF: failed to load page %d: %s",
               page, fz_caught_message (ctx));
  }

  if (pg) {
    /* Allocate the in-flight cancel cookie on this worker's stack and
     * publish its address so pdf_cancel_render can find it from the
     * main thread. The cookie is deregistered before this function
     * returns — pointer is never accessed after the worker's stack
     * frame goes away. */
    fz_cookie cookie = {0};
    g_mutex_lock (&self->cookies_lock);
    self->active_cookies[slot] = &cookie;
    g_mutex_unlock (&self->cookies_lock);

    /* Direct render into a fresh cairo surface — zero pixel copying. */
    surface = render_page_direct (ctx, (fz_page *) pg, zoom, rotation, &cookie);

    g_mutex_lock (&self->cookies_lock);
    self->active_cookies[slot] = NULL;
    g_mutex_unlock (&self->cookies_lock);

    fz_try (ctx) {
      fz_drop_page (ctx, (fz_page *) pg);
    }
    fz_catch (ctx) {
      /* best-effort drop */
    }
  }

  g_mutex_unlock (lock);

  FW_TRACE_PDF ("render_page done: page=%d slot=%d surface=%p",
                page, slot, (void *) surface);
  return surface;
}

/* ── TOC extraction ───────────────────────────────────────────────── */

static FwTocNode *
outline_to_toc (fz_context *ctx, fz_document *doc, fz_outline *outline)
{
  if (!outline)
    return NULL;

  FwTocNode *first = NULL;
  FwTocNode *prev  = NULL;

  for (fz_outline *ol = outline; ol; ol = ol->next) {
    int dest_page = -1;

    if (ol->page.page >= 0) {
      dest_page = ol->page.page;
    } else if (ol->uri) {
      fz_location loc = fz_resolve_link (ctx, doc, ol->uri, NULL, NULL);
      dest_page = loc.page;
    }

    FwTocNode *node = fw_toc_node_new (ol->title ? ol->title : "", dest_page);
    node->children = outline_to_toc (ctx, doc, ol->down);

    if (prev)
      prev->next = node;
    else
      first = node;

    prev = node;
  }

  return first;
}

static FwTocNode *
pdf_get_toc (FwDocument *doc)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  FwTocNode *result = NULL;
  fz_outline *outline = NULL;

  g_mutex_lock (&self->lock);

  fz_try (self->ctx) {
    outline = fz_load_outline (self->ctx, self->doc);
    result = outline_to_toc (self->ctx, self->doc, outline);
  }
  fz_always (self->ctx) {
    fz_drop_outline (self->ctx, outline);
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: failed to load TOC: %s",
               fz_caught_message (self->ctx));
  }

  g_mutex_unlock (&self->lock);

  return result;
}

/* ── Search ───────────────────────────────────────────────────────── */

/* Return the cached fz_stext_page for `page`, building it if absent.
 * Caller must hold self->lock. Returns NULL on extraction failure
 * (corrupt page, OOM, etc.). The cached entry is owned by self->ctx
 * and stays alive for the document's lifetime — read access from any
 * code path that already holds self->lock is safe.
 *
 * MuPDF's fz_stext_page is immutable after construction. We exploit
 * that to share a single cached copy across selection, search, and
 * (eventually) word/line click selection. */
static fz_stext_page *
pdf_get_or_extract_stext (FwDocumentPdf *self, int page)
{
  fz_stext_page *cached = g_hash_table_lookup (self->stext_cache,
                                                GINT_TO_POINTER (page));
  if (cached) return cached;

  volatile fz_page *pg = NULL;
  volatile fz_stext_page *stext = NULL;

  fz_try (self->ctx) {
    pg = fz_load_page (self->ctx, self->doc, page);
    stext = fz_new_stext_page_from_page (self->ctx, (fz_page *) pg, NULL);
  }
  fz_always (self->ctx) {
    if (pg) fz_drop_page (self->ctx, (fz_page *) pg);
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: stext extraction failed on page %d: %s",
               page, fz_caught_message (self->ctx));
    if (stext) {
      fz_drop_stext_page (self->ctx, (fz_stext_page *) stext);
      stext = NULL;
    }
  }

  if (stext)
    g_hash_table_insert (self->stext_cache, GINT_TO_POINTER (page),
                         (fz_stext_page *) stext);

  return (fz_stext_page *) stext;
}

static GArray *
pdf_search (FwDocument *doc, const char *text, int page)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  GArray *hits = g_array_new (FALSE, FALSE, sizeof (FwSearchHit));

  g_mutex_lock (&self->lock);

  fz_stext_page *stext = pdf_get_or_extract_stext (self, page);
  if (!stext) {
    g_mutex_unlock (&self->lock);
    return hits;
  }

  fz_try (self->ctx) {
    fz_quad quads[512];
    int count = fz_search_stext_page (self->ctx, stext, text, NULL, quads,
                                       G_N_ELEMENTS (quads));

    for (int i = 0; i < count; i++) {
      fz_rect r = fz_rect_from_quad (quads[i]);
      FwSearchHit hit = {
        .page = page,
        .x0 = (double) r.x0, .y0 = (double) r.y0,
        .x1 = (double) r.x1, .y1 = (double) r.y1,
      };
      g_array_append_val (hits, hit);
    }
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: search failed on page %d: %s",
               page, fz_caught_message (self->ctx));
  }

  g_mutex_unlock (&self->lock);

  return hits;
}

/* ── Text extraction ──────────────────────────────────────────────── */

static char *
pdf_get_text (FwDocument *doc, int page,
              double x0, double y0, double x1, double y1)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  char *result = NULL;

  g_mutex_lock (&self->lock);

  fz_stext_page *stext = pdf_get_or_extract_stext (self, page);
  if (!stext) {
    g_mutex_unlock (&self->lock);
    return NULL;
  }

  fz_try (self->ctx) {
    fz_point a = { (float) x0, (float) y0 };
    fz_point b = { (float) x1, (float) y1 };
    char *mupdf_text = fz_copy_selection (self->ctx, stext, a, b, 0);
    if (mupdf_text) {
      result = g_strdup (mupdf_text);
      fz_free (self->ctx, mupdf_text);
    }
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: text extraction failed on page %d: %s",
               page, fz_caught_message (self->ctx));
  }

  g_mutex_unlock (&self->lock);

  return result;
}

/* ── Click-to-select snap ─────────────────────────────────────────── */

static gboolean
pdf_select_at (FwDocument *doc, int page, double x, double y,
               FwSelectGranularity gran,
               double *out_x0, double *out_y0,
               double *out_x1, double *out_y1)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  gboolean ok = FALSE;

  /* MuPDF's snap mode constants. fz_snap_selection takes both endpoints
   * by reference and returns a quad covering the snapped region. With
   * a == b set to the click point, it picks the word/line containing
   * that point. */
  int mode = (gran == FW_SELECT_LINE) ? FZ_SELECT_LINES : FZ_SELECT_WORDS;

  g_mutex_lock (&self->lock);

  fz_stext_page *stext = pdf_get_or_extract_stext (self, page);
  if (!stext) {
    g_mutex_unlock (&self->lock);
    return FALSE;
  }

  fz_try (self->ctx) {
    fz_point a = { (float) x, (float) y };
    fz_point b = { (float) x, (float) y };
    fz_quad q = fz_snap_selection (self->ctx, stext, &a, &b, mode);

    /* If the click missed every glyph, fz_snap_selection returns a
     * zero-size or default quad — detect that and report failure so the
     * caller can leave the existing selection alone. */
    float w = q.lr.x - q.ul.x;
    float h = q.lr.y - q.ul.y;
    if (w > 0.5f && h > 0.5f) {
      *out_x0 = (double) q.ul.x;
      *out_y0 = (double) q.ul.y;
      *out_x1 = (double) q.lr.x;
      *out_y1 = (double) q.lr.y;
      ok = TRUE;
    }
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: snap selection failed on page %d: %s",
               page, fz_caught_message (self->ctx));
  }

  g_mutex_unlock (&self->lock);
  return ok;
}

static GArray *
pdf_get_selection_quads (FwDocument *doc, int page,
                         double x0, double y0, double x1, double y1)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  GArray *out = g_array_new (FALSE, FALSE, sizeof (FwRect));

  g_mutex_lock (&self->lock);

  fz_stext_page *stext = pdf_get_or_extract_stext (self, page);
  if (!stext) {
    g_mutex_unlock (&self->lock);
    return out;
  }

  /* fz_highlight_selection follows reading order: partial first line +
   * full middle lines + partial last line, returned as one quad per
   * line. The 256-quad bound covers single-page selections of the
   * largest densely-typeset textbook pages (~50-line printed page); a
   * per-document worst case far below this. */
  fz_quad quads[256];
  fz_try (self->ctx) {
    fz_point a = { (float) x0, (float) y0 };
    fz_point b = { (float) x1, (float) y1 };
    int n = fz_highlight_selection (self->ctx, stext, a, b,
                                     quads, G_N_ELEMENTS (quads));
    for (int i = 0; i < n; i++) {
      /* MuPDF returns a fz_quad (4 corners); for our axis-aligned
       * highlight overlay the bounding box is enough. */
      fz_rect r = fz_rect_from_quad (quads[i]);
      FwRect rect = {
        .x0 = (double) r.x0, .y0 = (double) r.y0,
        .x1 = (double) r.x1, .y1 = (double) r.y1,
      };
      g_array_append_val (out, rect);
    }
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: highlight selection failed on page %d: %s",
               page, fz_caught_message (self->ctx));
  }

  g_mutex_unlock (&self->lock);
  return out;
}

/* ── Content bbox (margin-crop support) ──────────────────────────── */

static gboolean
pdf_get_content_bbox (FwDocument *doc, int page,
                      double *out_x0, double *out_y0,
                      double *out_x1, double *out_y1)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  gboolean ok = FALSE;

  g_mutex_lock (&self->lock);

  fz_stext_page *stext = pdf_get_or_extract_stext (self, page);
  if (!stext) {
    g_mutex_unlock (&self->lock);
    return FALSE;
  }

  /* Walk every char in every line in every text block, unioning its
   * quad-derived bbox. Image blocks are skipped (margin-crop is for
   * trimming text-doc whitespace; pages that are pure images would
   * need a per-pixel detector, out of scope here). */
  fz_rect bbox = fz_empty_rect;
  fz_try (self->ctx) {
    for (fz_stext_block *block = stext->first_block; block; block = block->next) {
      if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
      for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
        for (fz_stext_char *ch = line->first_char; ch; ch = ch->next) {
          fz_rect r = fz_rect_from_quad (ch->quad);
          bbox = fz_union_rect (bbox, r);
        }
      }
    }
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: content bbox walk failed on page %d: %s",
               page, fz_caught_message (self->ctx));
    bbox = fz_empty_rect;
  }

  /* Empty bbox (no text on page, e.g., a chapter divider) → fail.
   * The view falls back to the full page rect. */
  if (bbox.x1 > bbox.x0 && bbox.y1 > bbox.y0) {
    *out_x0 = (double) bbox.x0;
    *out_y0 = (double) bbox.y0;
    *out_x1 = (double) bbox.x1;
    *out_y1 = (double) bbox.y1;
    ok = TRUE;
  }

  g_mutex_unlock (&self->lock);
  return ok;
}

/* ── Link extraction ──────────────────────────────────────────────── */

static GArray *
pdf_get_links (FwDocument *doc, int page)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  GArray *links = g_array_new (FALSE, FALSE, sizeof (FwLink *));
  g_array_set_clear_func (links, (GDestroyNotify) fw_link_free_indirect);
  fz_page *pg = NULL;
  fz_link *fz_links = NULL;

  g_mutex_lock (&self->lock);

  fz_try (self->ctx) {
    pg = fz_load_page (self->ctx, self->doc, page);
    fz_links = fz_load_links (self->ctx, pg);

    for (fz_link *fl = fz_links; fl; fl = fl->next) {
      fz_rect r = fl->rect;
      FwLink *link = NULL;

      if (fz_is_external_link (self->ctx, fl->uri)) {
        link = fw_link_new_external ((double) r.x0, (double) r.y0,
                                     (double) r.x1, (double) r.y1,
                                     fl->uri);
      } else {
        fz_location loc = fz_resolve_link (self->ctx, self->doc,
                                            fl->uri, NULL, NULL);
        link = fw_link_new_internal ((double) r.x0, (double) r.y0,
                                     (double) r.x1, (double) r.y1,
                                     loc.page);
      }

      g_array_append_val (links, link);
    }
  }
  fz_always (self->ctx) {
    fz_drop_link (self->ctx, fz_links);
    fz_drop_page (self->ctx, pg);
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: failed to load links on page %d: %s",
               page, fz_caught_message (self->ctx));
  }

  g_mutex_unlock (&self->lock);

  return links;
}

/* ── Page handle API ─────────────────────────────────────────────── */
/* PDF rendering uses independent document instances (see pdf_render_page).
 * The page handle API is not used — return NULL so the cache falls back
 * to fw_document_render_page which dispatches to the parallel instances. */

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_document_pdf_dispose (GObject *object)
{
  pdf_close (FW_DOCUMENT (object));
  G_OBJECT_CLASS (fw_document_pdf_parent_class)->dispose (object);
}

static void
fw_document_pdf_finalize (GObject *object)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (object);
  g_mutex_clear (&self->lock);
  g_mutex_clear (&self->cookies_lock);
  if (self->stext_cache) {
    g_hash_table_unref (self->stext_cache);
    self->stext_cache = NULL;
  }
  G_OBJECT_CLASS (fw_document_pdf_parent_class)->finalize (object);
}

static void
fw_document_pdf_class_init (FwDocumentPdfClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose  = fw_document_pdf_dispose;
  object_class->finalize = fw_document_pdf_finalize;
}

static void
fw_document_pdf_init (FwDocumentPdf *self)
{
  g_mutex_init (&self->lock);
  g_mutex_init (&self->cookies_lock);
  for (int i = 0; i < MAX_RENDER_INSTANCES; i++)
    self->active_cookies[i] = NULL;
  /* No GDestroyNotify — entries are fz_stext_page* owned by self->ctx,
   * which is dropped after the cache is cleared in pdf_close. */
  self->stext_cache = g_hash_table_new (g_direct_hash, g_direct_equal);
}

/* ── Embedded files (PDF /EmbeddedFiles name tree) ───────────────── */

/* Sanitize an attacker-supplied filename: strip directory components,
 * collapse "..", drop leading dots, and replace control chars with '_'.
 * The result is safe to join with a user-chosen output directory. */
static char *
sanitize_basename (const char *raw)
{
  if (!raw || !raw[0])
    return g_strdup ("attachment");

  /* Take the basename only — drop "../" / "/foo/" path traversal. */
  const char *slash1 = strrchr (raw, '/');
  const char *slash2 = strrchr (raw, '\\');
  const char *base = raw;
  if (slash1 && slash1 + 1 > base) base = slash1 + 1;
  if (slash2 && slash2 + 1 > base) base = slash2 + 1;

  /* Reject empty / dot-only names. */
  if (!base[0] || g_strcmp0 (base, ".") == 0 || g_strcmp0 (base, "..") == 0)
    return g_strdup ("attachment");

  GString *out = g_string_new (NULL);
  for (const char *p = base; *p; p++) {
    unsigned char c = (unsigned char) *p;
    if (c < 0x20 || c == 0x7f)
      g_string_append_c (out, '_');
    else
      g_string_append_c (out, *p);
  }
  /* Strip leading dots — prevents creating dotfiles by surprise. */
  while (out->len && out->str[0] == '.')
    g_string_erase (out, 0, 1);
  if (!out->len)
    g_string_assign (out, "attachment");
  return g_string_free (out, FALSE);
}

static GArray *
pdf_get_attachments (FwDocument *doc)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  GArray *out = g_array_new (FALSE, FALSE, sizeof (FwAttachment *));
  g_array_set_clear_func (out, fw_attachment_free_indirect);

  g_mutex_lock (&self->lock);

  pdf_document *pdoc = pdf_specifics (self->ctx, self->doc);
  if (!pdoc) {
    g_mutex_unlock (&self->lock);
    return out;
  }

  fz_try (self->ctx) {
    int n = pdf_xref_len (self->ctx, pdoc);
    for (int i = 1; i < n; i++) {
      pdf_obj *obj = NULL;
      fz_try (self->ctx) {
        obj = pdf_load_object (self->ctx, pdoc, i);
        if (obj && pdf_is_embedded_file (self->ctx, obj)) {
          pdf_filespec_params p = { 0 };
          pdf_get_filespec_params (self->ctx, obj, &p);
          FwAttachment *a = g_new0 (FwAttachment, 1);
          a->name = sanitize_basename (p.filename);
          a->mime_type = p.mimetype ? g_strdup (p.mimetype) : NULL;
          a->size = p.size;
          a->backend_data = GINT_TO_POINTER (i);  /* xref index */
          g_array_append_val (out, a);
        }
      }
      fz_always (self->ctx) {
        if (obj) pdf_drop_obj (self->ctx, obj);
      }
      fz_catch (self->ctx) {
        /* Skip this xref entry on any error; keep iterating. */
      }
    }
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: get_attachments failed: %s",
               fz_caught_message (self->ctx));
  }

  g_mutex_unlock (&self->lock);
  return out;
}

static gboolean
pdf_save_attachment (FwDocument *doc, FwAttachment *attachment,
                     const char *output_path, GError **error)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  int xref_idx = GPOINTER_TO_INT (attachment->backend_data);

  if (xref_idx < 1) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "Invalid attachment handle");
    return FALSE;
  }

  g_mutex_lock (&self->lock);

  pdf_document *pdoc = pdf_specifics (self->ctx, self->doc);
  if (!pdoc) {
    g_mutex_unlock (&self->lock);
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Document is not a PDF");
    return FALSE;
  }

  pdf_obj    *obj = NULL;
  fz_buffer  *buf = NULL;
  volatile gboolean ok = FALSE;
  const char *errmsg = NULL;

  fz_try (self->ctx) {
    obj = pdf_load_object (self->ctx, pdoc, xref_idx);
    if (!obj || !pdf_is_embedded_file (self->ctx, obj))
      fz_throw (self->ctx, FZ_ERROR_GENERIC, "object is not an embedded file");
    buf = pdf_load_embedded_file_contents (self->ctx, obj);
    if (!buf)
      fz_throw (self->ctx, FZ_ERROR_GENERIC, "no embedded contents");
    fz_save_buffer (self->ctx, buf, output_path);
    ok = TRUE;
  }
  fz_always (self->ctx) {
    if (buf) fz_drop_buffer (self->ctx, buf);
    if (obj) pdf_drop_obj (self->ctx, obj);
  }
  fz_catch (self->ctx) {
    errmsg = fz_caught_message (self->ctx);
  }

  g_mutex_unlock (&self->lock);

  if (!ok) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "MuPDF: %s", errmsg ? errmsg : "unknown failure");
    return FALSE;
  }
  return TRUE;
}

/* ── Metadata extraction ─────────────────────────────────────────── */

/* Parse a PDF date string ("D:YYYYMMDDHHmmSSOHH'mm'") into a human-readable
 * "YYYY-MM-DD HH:MM:SS [+/-HH:MM]" form. Returns NULL if the input doesn't
 * match the expected shape — the dialog will then fall back to the raw
 * value. PDF dates are optional past the year, so any tail is best-effort. */
static char *
format_pdf_date (const char *s)
{
  if (!s) return NULL;
  if (s[0] == 'D' && s[1] == ':') s += 2;

  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  int n = sscanf (s, "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &se);
  if (n < 1 || y < 1900 || y > 9999) return NULL;
  if (n < 2) mo = 1;
  if (n < 3) d  = 1;

  /* Skip past the parts we already consumed to inspect the timezone. */
  size_t consumed = (size_t) (n * 2);
  if (n >= 1) consumed += 2;  /* year is 4 chars, not 2 */
  const char *tz = (strlen (s) > consumed) ? s + consumed : NULL;

  GString *out = g_string_new (NULL);
  if (n >= 4)
    g_string_append_printf (out, "%04d-%02d-%02d %02d:%02d:%02d",
                            y, mo, d, h, mi, se);
  else
    g_string_append_printf (out, "%04d-%02d-%02d", y, mo, d);

  if (tz && (*tz == '+' || *tz == '-')) {
    int oh = 0, om = 0;
    if (sscanf (tz + 1, "%2d'%2d", &oh, &om) >= 1)
      g_string_append_printf (out, " %c%02d:%02d", *tz, oh, om);
  } else if (tz && *tz == 'Z') {
    g_string_append (out, " UTC");
  }
  return g_string_free (out, FALSE);
}

static void
metadata_lookup (fz_context *ctx, fz_document *doc,
                 GHashTable *out, const char *mupdf_key,
                 const char *out_key, gboolean is_date)
{
  char buf[1024];
  int n = fz_lookup_metadata (ctx, doc, mupdf_key, buf, sizeof buf);
  if (n <= 0) return;
  /* Trim trailing whitespace and reject empty values. */
  while (n > 0 && g_ascii_isspace (buf[n - 1])) buf[--n] = '\0';
  if (n == 0) return;

  char *val = NULL;
  if (is_date) val = format_pdf_date (buf);
  if (!val)    val = g_strdup (buf);
  g_hash_table_insert (out, g_strdup (out_key), val);
}

static GHashTable *
pdf_get_metadata (FwDocument *doc)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  GHashTable *out = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, g_free);

  g_mutex_lock (&self->lock);
  fz_try (self->ctx) {
    metadata_lookup (self->ctx, self->doc, out, "info:Title",        "title",             FALSE);
    metadata_lookup (self->ctx, self->doc, out, "info:Author",       "author",            FALSE);
    metadata_lookup (self->ctx, self->doc, out, "info:Subject",      "subject",           FALSE);
    metadata_lookup (self->ctx, self->doc, out, "info:Keywords",     "keywords",          FALSE);
    metadata_lookup (self->ctx, self->doc, out, "info:Creator",      "creator",           FALSE);
    metadata_lookup (self->ctx, self->doc, out, "info:Producer",     "producer",          FALSE);
    metadata_lookup (self->ctx, self->doc, out, "info:CreationDate", "creation-date",     TRUE);
    metadata_lookup (self->ctx, self->doc, out, "info:ModDate",      "modification-date", TRUE);
    metadata_lookup (self->ctx, self->doc, out, FZ_META_FORMAT,      "format",            FALSE);
    metadata_lookup (self->ctx, self->doc, out, FZ_META_ENCRYPTION,  "encryption",        FALSE);
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: get_metadata failed: %s",
               fz_caught_message (self->ctx));
  }
  g_mutex_unlock (&self->lock);

  if (g_hash_table_size (out) == 0) {
    g_hash_table_unref (out);
    return NULL;
  }
  return out;
}

/* In-flight cancel: walk the active_cookies array under cookies_lock and
 * flip ->abort = 1 on every published cookie. The render workers see the
 * flag at the next fz_run_page checkpoint and bail with whatever partial
 * state they have. cookies_lock is independent of the per-instance render
 * lock, so cancel never blocks waiting for a render to finish — it just
 * tells MuPDF to *make* it finish, fast.
 *
 * Called from FwCache when the velocity engine transitions to SCRUBBING
 * (`fw_cache_set_velocity`) and on document close. */
static void
pdf_cancel_render (FwDocument *doc)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  int aborted = 0;
  g_mutex_lock (&self->cookies_lock);
  for (int i = 0; i < self->n_render; i++) {
    if (self->active_cookies[i]) {
      self->active_cookies[i]->abort = 1;
      aborted++;
    }
  }
  g_mutex_unlock (&self->cookies_lock);
  if (aborted)
    FW_TRACE_PDF ("cancel_render: aborted %d in-flight render(s)", aborted);
}

static void
fw_document_pdf_iface_init (FwDocumentInterface *iface)
{
  iface->open            = pdf_open;
  iface->close           = pdf_close;
  iface->get_page_count  = pdf_get_page_count;
  iface->get_page_size   = pdf_get_page_size;
  iface->render_page     = pdf_render_page;
  iface->cancel_render   = pdf_cancel_render;
  iface->get_toc         = pdf_get_toc;
  iface->search          = pdf_search;
  iface->get_text        = pdf_get_text;
  iface->get_links       = pdf_get_links;
  iface->get_attachments = pdf_get_attachments;
  iface->save_attachment = pdf_save_attachment;
  iface->get_metadata    = pdf_get_metadata;
  iface->select_at       = pdf_select_at;
  iface->get_selection_quads = pdf_get_selection_quads;
  iface->get_content_bbox    = pdf_get_content_bbox;
  /* Page handle API intentionally NULL — PDF uses independent render instances
   * in pdf_render_page() for true parallel rendering. The cache falls through
   * to fw_document_render_page() which dispatches here. */
}

FwDocumentPdf *
fw_document_pdf_new (void)
{
  return g_object_new (FW_TYPE_DOCUMENT_PDF, NULL);
}
