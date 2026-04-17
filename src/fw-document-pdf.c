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
 * Called with inst->lock held. */
static cairo_surface_t *
render_page_direct (fz_context *ctx, fz_page *page,
                    double zoom, int rotation)
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
    fz_run_page (ctx, page, (fz_device *) dev, draw_m, NULL);
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

/* ── Interface implementation ─────────────────────────────────────── */

static gboolean
pdf_open (FwDocument *doc, const char *path, GError **error)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);

  fz_context *ctx = fz_new_context (NULL, NULL, 64 << 20);
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
    self->render[i].ctx = fz_new_context (NULL, NULL, 64 << 20);
    if (self->render[i].ctx) {
      fz_set_warning_callback (self->render[i].ctx, pdf_warn_handler, NULL);
      fz_set_error_callback (self->render[i].ctx, pdf_error_handler, NULL);
      fz_register_document_handlers (self->render[i].ctx);
      fz_try (self->render[i].ctx) {
        self->render[i].doc = fz_open_document (self->render[i].ctx, path);
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
    /* Direct render into a fresh cairo surface — zero pixel copying. */
    surface = render_page_direct (ctx, (fz_page *) pg, zoom, rotation);
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

static GArray *
pdf_search (FwDocument *doc, const char *text, int page)
{
  FwDocumentPdf *self = FW_DOCUMENT_PDF (doc);
  GArray *hits = g_array_new (FALSE, FALSE, sizeof (FwSearchHit));
  fz_page *pg = NULL;

  g_mutex_lock (&self->lock);

  fz_try (self->ctx) {
    pg = fz_load_page (self->ctx, self->doc, page);

    fz_quad quads[512];
    int count = fz_search_page (self->ctx, pg, text, NULL, quads,
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
  fz_always (self->ctx) {
    fz_drop_page (self->ctx, pg);
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
  fz_page *pg = NULL;
  fz_stext_page *stext = NULL;

  g_mutex_lock (&self->lock);

  fz_try (self->ctx) {
    pg = fz_load_page (self->ctx, self->doc, page);
    stext = fz_new_stext_page_from_page (self->ctx, pg, NULL);
    fz_point a = { (float) x0, (float) y0 };
    fz_point b = { (float) x1, (float) y1 };
    char *mupdf_text = fz_copy_selection (self->ctx, stext, a, b, 0);
    if (mupdf_text) {
      result = g_strdup (mupdf_text);
      fz_free (self->ctx, mupdf_text);
    }
  }
  fz_always (self->ctx) {
    fz_drop_stext_page (self->ctx, stext);
    fz_drop_page (self->ctx, pg);
  }
  fz_catch (self->ctx) {
    g_warning ("MuPDF: text extraction failed on page %d: %s",
               page, fz_caught_message (self->ctx));
  }

  g_mutex_unlock (&self->lock);

  return result;
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
}

static void
fw_document_pdf_iface_init (FwDocumentInterface *iface)
{
  iface->open           = pdf_open;
  iface->close          = pdf_close;
  iface->get_page_count = pdf_get_page_count;
  iface->get_page_size  = pdf_get_page_size;
  iface->render_page    = pdf_render_page;
  iface->get_toc        = pdf_get_toc;
  iface->search         = pdf_search;
  iface->get_text       = pdf_get_text;
  iface->get_links      = pdf_get_links;
  /* Page handle API intentionally NULL — PDF uses independent render instances
   * in pdf_render_page() for true parallel rendering. The cache falls through
   * to fw_document_render_page() which dispatches here. */
}

FwDocumentPdf *
fw_document_pdf_new (void)
{
  return g_object_new (FW_TYPE_DOCUMENT_PDF, NULL);
}
