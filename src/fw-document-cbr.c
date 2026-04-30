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
};

static void fw_document_cbr_iface_init (FwDocumentInterface *iface);

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

/* Extract the entry whose name matches self->entries[page].name into a
 * freshly malloced buffer. Caller frees with g_free. Returns NULL on
 * error or cancellation. */
static guint8 *
cbr_extract_entry (FwDocumentCbr *self, int page, size_t *out_size)
{
  if (page < 0 || page >= self->page_count)
    return NULL;
  *out_size = 0;

  g_mutex_lock (&self->archive_lock);

  if (g_atomic_int_get (&self->cancel_flag)) {
    g_mutex_unlock (&self->archive_lock);
    return NULL;
  }

  struct archive *a = cbr_open_reader (self);
  if (!a) {
    g_mutex_unlock (&self->archive_lock);
    return NULL;
  }

  const char *target = self->entries[page].name;
  guint8 *bytes = NULL;
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
      *out_size = (size_t) sz;
    break;
  }

  archive_read_free (a);
  g_mutex_unlock (&self->archive_lock);
  return bytes;
}

/* Render one image entry into a freshly allocated cairo ARGB32 surface,
 * applying zoom + rotation. Uses MuPDF's draw device wrapping cairo's
 * pixel buffer — the same zero-copy trick the PDF backend uses. */
static cairo_surface_t *
cbr_render (FwDocumentCbr *self, int page, double zoom, int rotation)
{
  size_t sz = 0;
  guint8 *bytes = cbr_extract_entry (self, page, &sz);
  if (!bytes)
    return NULL;

  if (g_atomic_int_get (&self->cancel_flag)) {
    g_free (bytes);
    return NULL;
  }

  cairo_surface_t *surface = NULL;

  g_mutex_lock (&self->ctx_lock);

  fz_buffer  *buf       = NULL;
  fz_image   *img       = NULL;
  fz_pixmap  *cairo_pix = NULL;
  fz_device  *draw_dev  = NULL;

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
    if (w < 1 || h < 1)
      fz_throw (self->ctx, FZ_ERROR_GENERIC, "zero-size render");

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
    g_warning ("CBR: render failed page %d: %s",
               page, fz_caught_message (self->ctx));
    if (surface) {
      cairo_surface_destroy (surface);
      surface = NULL;
    }
  }

  g_mutex_unlock (&self->ctx_lock);

  g_free (bytes);
  return surface;
}

/* ── FwDocumentInterface implementation ───────────────────────────── */

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

  self->page_widths  = g_new (double, self->page_count);
  self->page_heights = g_new (double, self->page_count);

  /* Phase 2: peek at page 0 for representative dimensions; use as the
   * default for every page. Each page's actual dims override this when
   * it first renders. For 99% of comics every page is the same size, so
   * the default is correct from the start. */
  double default_w = 1280, default_h = 1920;
  size_t first_sz = 0;
  guint8 *first_bytes = cbr_extract_entry (self, 0, &first_sz);
  if (first_bytes) {
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
    g_free (first_bytes);
  }

  for (int i = 0; i < self->page_count; i++) {
    self->page_widths[i]  = default_w;
    self->page_heights[i] = default_h;
  }

  FW_TRACE_DOC ("cbr open: '%s' %d pages, default size %.0fx%.0f",
                path, self->page_count, default_w, default_h);
  return TRUE;
}

static void
cbr_close (FwDocument *doc)
{
  FwDocumentCbr *self = FW_DOCUMENT_CBR (doc);

  if (self->entries) {
    for (int i = 0; i < self->page_count; i++)
      g_free (self->entries[i].name);
    g_free (self->entries);
    self->entries = NULL;
  }
  g_clear_pointer (&self->page_widths,  g_free);
  g_clear_pointer (&self->page_heights, g_free);
  g_clear_pointer (&self->path,         g_free);
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
}

FwDocumentCbr *
fw_document_cbr_new (void)
{
  return g_object_new (FW_TYPE_DOCUMENT_CBR, NULL);
}
