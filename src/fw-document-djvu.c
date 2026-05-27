/* fw-document-djvu.c — DjVuLibre backend implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document-djvu.h"
#include "fw-debug.h"

#include <gio/gio.h>
#include <libdjvu/ddjvuapi.h>
#include <libdjvu/miniexp.h>
#include <stdint.h>
#include <string.h>

struct _FwDocumentDjvu {
  GObject            parent_instance;

  ddjvu_context_t   *djvu_ctx;
  ddjvu_document_t  *djvu_doc;
  char              *path;
  int                page_count;
  GMutex             render_lock;  /* DjVuLibre is not thread-safe */
  /* Cancel uses a monotonic generation counter, not a flag. Workers
   * capture cancel_gen at entry and bail at any later checkpoint
   * where the value differs. Avoids the "TRUE-set-then-FALSE-reset"
   * race in the previous flag-based design. djvu_cancel_render
   * increments; only the increment is racy, and increments compose
   * cleanly. */
  volatile gint      cancel_gen;
  ddjvu_format_t    *render_fmt;   /* cached RGBMASK32 format for cairo ARGB32 */

  /* Page sizes cached at open time (in points, 72 DPI) */
  double            *page_widths;
  double            *page_heights;
};

static void fw_document_djvu_iface_init (FwDocumentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwDocumentDjvu, fw_document_djvu, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE (FW_TYPE_DOCUMENT, fw_document_djvu_iface_init))

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Pump the DjVuLibre message queue until a given condition is met.
 * DjVuLibre decodes asynchronously — we must wait for completion. */
static void
djvu_wait_for_job (ddjvu_context_t *ctx, ddjvu_job_t *job)
{
  while (!ddjvu_job_done (job)) {
    const ddjvu_message_t *msg = ddjvu_message_wait (ctx);
    if (msg)
      ddjvu_message_pop (ctx);
  }
}

static void
djvu_drain_messages (ddjvu_context_t *ctx)
{
  const ddjvu_message_t *msg;
  while ((msg = ddjvu_message_peek (ctx)))
    ddjvu_message_pop (ctx);
}

/* Render a DjVu page directly into a cairo ARGB32 surface.
 *
 * Uses DDJVU_FORMAT_RGBMASK32 with masks matching cairo's native ARGB32
 * layout (0x00RRGGBB).  DjVuLibre writes 32-bit pixels directly into the
 * cairo buffer — no temporary allocation, no pixel-by-pixel conversion.
 * The alpha channel (always opaque) is filled in a fast OR pass that runs
 * OUTSIDE the render lock. */
static cairo_surface_t *
djvu_render_to_cairo (ddjvu_format_t *fmt, ddjvu_page_t *page,
                      int w, int h, gboolean *need_alpha)
{
  cairo_surface_t *surface =
    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);

  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy (surface);
    *need_alpha = FALSE;
    return NULL;
  }

  cairo_surface_flush (surface);

  unsigned char *cairo_data = cairo_image_surface_get_data (surface);
  int cairo_stride = cairo_image_surface_get_stride (surface);

  ddjvu_rect_t rect = { 0, 0, (unsigned int) w, (unsigned int) h };

  int ok = ddjvu_page_render (page, DDJVU_RENDER_COLOR,
                               &rect, &rect, fmt,
                               (unsigned long) cairo_stride,
                               (char *) cairo_data);

  if (!ok) {
    /* Render failed — return white opaque surface */
    memset (cairo_data, 0xFF, (size_t) cairo_stride * (size_t) h);
    cairo_surface_mark_dirty (surface);
    *need_alpha = FALSE;
    return surface;
  }

  /* Alpha will be filled outside the lock for less contention */
  *need_alpha = TRUE;
  return surface;
}

/* Set alpha to 0xFF on every pixel.  DjVuLibre's RGBMASK32 leaves the
 * high byte as 0x00 — we OR in the alpha.  This is fast and trivially
 * vectorized by the compiler. */
static void
djvu_fill_alpha (cairo_surface_t *surface)
{
  unsigned char *data = cairo_image_surface_get_data (surface);
  int stride = cairo_image_surface_get_stride (surface);
  int h = cairo_image_surface_get_height (surface);
  int w = cairo_image_surface_get_width (surface);

  for (int y = 0; y < h; y++) {
    uint32_t *row = (uint32_t *) (data + y * stride);
    for (int x = 0; x < w; x++)
      row[x] |= 0xFF000000u;
  }

  cairo_surface_mark_dirty (surface);
}

/* ── Interface implementation ─────────────────────────────────────── */

static gboolean
djvu_open (FwDocument *doc, const char *path, GError **error)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);

  self->djvu_ctx = ddjvu_context_create ("framework");
  if (!self->djvu_ctx) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Failed to create DjVu context");
    return FALSE;
  }

  self->djvu_doc = ddjvu_document_create_by_filename (self->djvu_ctx,
                                                       path, TRUE);
  if (!self->djvu_doc) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "DjVu: failed to open %s", path);
    ddjvu_context_release (self->djvu_ctx);
    self->djvu_ctx = NULL;
    return FALSE;
  }

  /* Wait for document decoding to complete */
  djvu_wait_for_job (self->djvu_ctx,
                     ddjvu_document_job (self->djvu_doc));
  djvu_drain_messages (self->djvu_ctx);

  if (ddjvu_document_decoding_error (self->djvu_doc)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "DjVu: decoding error for %s", path);
    ddjvu_document_release (self->djvu_doc);
    ddjvu_context_release (self->djvu_ctx);
    self->djvu_doc = NULL;
    self->djvu_ctx = NULL;
    return FALSE;
  }

  self->page_count = ddjvu_document_get_pagenum (self->djvu_doc);
  self->path = g_strdup (path);
  FW_TRACE_DJVU ("opened '%s': %d pages", path, self->page_count);

  /* Create a reusable render format that matches cairo's ARGB32 layout.
   * RGBMASK32 renders 32-bit pixels with caller-specified channel masks,
   * letting DjVuLibre write directly into the cairo surface buffer. */
  {
    unsigned int masks[3] = { 0x00FF0000, 0x0000FF00, 0x000000FF };
    self->render_fmt = ddjvu_format_create (DDJVU_FORMAT_RGBMASK32, 3, masks);
    ddjvu_format_set_row_order (self->render_fmt, 1);  /* top-to-bottom */
  }

  /* Pre-cache all page dimensions to avoid repeated ddjvu_document_get_pageinfo
   * calls during layout. Matches the PDF backend's fast probing strategy. */
  self->page_widths  = g_new (double, self->page_count);
  self->page_heights = g_new (double, self->page_count);

  for (int i = 0; i < self->page_count; i++) {
    self->page_widths[i]  = 612.0;  /* fallback: US Letter */
    self->page_heights[i] = 792.0;

    ddjvu_pageinfo_t info;
    ddjvu_status_t status = ddjvu_document_get_pageinfo (self->djvu_doc,
                                                          i, &info);
    if (status == DDJVU_JOB_OK) {
      double dpi = (double) info.dpi;
      if (dpi <= 0) dpi = 300.0;
      self->page_widths[i]  = (double) info.width  * 72.0 / dpi;
      self->page_heights[i] = (double) info.height * 72.0 / dpi;
    }
  }

  return TRUE;
}

static void
djvu_close (FwDocument *doc)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);
  FW_TRACE_DJVU ("close: '%s'", self->path ? self->path : "(null)");

  if (self->render_fmt) {
    ddjvu_format_release (self->render_fmt);
    self->render_fmt = NULL;
  }
  if (self->djvu_doc) {
    ddjvu_document_release (self->djvu_doc);
    self->djvu_doc = NULL;
  }
  if (self->djvu_ctx) {
    ddjvu_context_release (self->djvu_ctx);
    self->djvu_ctx = NULL;
  }
  g_clear_pointer (&self->path, g_free);
  g_clear_pointer (&self->page_widths, g_free);
  g_clear_pointer (&self->page_heights, g_free);
  self->page_count = 0;
}

static int
djvu_get_page_count (FwDocument *doc)
{
  return FW_DOCUMENT_DJVU (doc)->page_count;
}

static void
djvu_get_page_size (FwDocument *doc, int page,
                    double *width, double *height)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);

  if (page >= 0 && page < self->page_count && self->page_widths) {
    if (width)  *width  = self->page_widths[page];
    if (height) *height = self->page_heights[page];
  } else {
    if (width)  *width  = 612.0;
    if (height) *height = 792.0;
  }
}

/* Apply rotation to a cairo surface via a new rotated surface + cairo transform */
static cairo_surface_t *
djvu_apply_rotation (cairo_surface_t *surface, int w, int h, int rotation)
{
  if (rotation == 0 || !surface)
    return surface;

  int rw = w, rh = h;
  if (rotation == 90 || rotation == 270) {
    rw = h;
    rh = w;
  }

  cairo_surface_t *rotated =
    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, rw, rh);
  cairo_t *cr = cairo_create (rotated);

  if (rotation == 90) {
    cairo_translate (cr, rw, 0);
  } else if (rotation == 180) {
    cairo_translate (cr, rw, rh);
  } else if (rotation == 270) {
    cairo_translate (cr, 0, rh);
  }
  cairo_rotate (cr, rotation * G_PI / 180.0);
  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);

  cairo_surface_destroy (surface);
  return rotated;
}

static cairo_surface_t *
djvu_render_page (FwDocument *doc, int page, double zoom, int rotation)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);
  cairo_surface_t *surface = NULL;

  FW_TRACE_DJVU ("render_page start: page=%d zoom=%.2f rot=%d", page, zoom, rotation);
  /* Capture the cancel generation BEFORE acquiring the lock — if a
   * concurrent cancel happens between this read and our work, the
   * checkpoint reads will see a different gen and bail. */
  gint start_gen = g_atomic_int_get (&self->cancel_gen);
  g_mutex_lock (&self->render_lock);

  if (g_atomic_int_get (&self->cancel_gen) != start_gen) {
    g_mutex_unlock (&self->render_lock);
    FW_TRACE_DJVU ("render_page cancelled (pre-decode): page=%d", page);
    return NULL;
  }

  ddjvu_page_t *pg = ddjvu_page_create_by_pageno (self->djvu_doc, page);
  if (!pg) {
    g_mutex_unlock (&self->render_lock);
    FW_TRACE_DJVU ("render_page FAILED (create): page=%d", page);
    return NULL;
  }

  djvu_wait_for_job (self->djvu_ctx, ddjvu_page_job (pg));
  djvu_drain_messages (self->djvu_ctx);

  if (g_atomic_int_get (&self->cancel_gen) != start_gen) {
    ddjvu_page_release (pg);
    g_mutex_unlock (&self->render_lock);
    FW_TRACE_DJVU ("render_page cancelled (post-decode): page=%d", page);
    return NULL;
  }

  if (ddjvu_page_decoding_error (pg)) {
    g_warning ("DjVu: decoding error on page %d", page);
    ddjvu_page_release (pg);
    g_mutex_unlock (&self->render_lock);
    FW_TRACE_DJVU ("render_page FAILED (decode error): page=%d", page);
    return NULL;
  }

  /* Compute pixel dimensions */
  int dpi = ddjvu_page_get_resolution (pg);
  if (dpi <= 0) dpi = 300;

  int orig_w = ddjvu_page_get_width (pg);
  int orig_h = ddjvu_page_get_height (pg);

  /* zoom=1.0 means render at 72 DPI (1 point = 1 pixel) */
  double scale = zoom * 72.0 / (double) dpi;
  int w = (int) ((double) orig_w * scale + 0.5);
  int h = (int) ((double) orig_h * scale + 0.5);

  if (w < 1) w = 1;
  if (h < 1) h = 1;

  gboolean need_alpha = FALSE;
  surface = djvu_render_to_cairo (self->render_fmt, pg, w, h, &need_alpha);
  ddjvu_page_release (pg);
  g_mutex_unlock (&self->render_lock);

  /* Alpha fill and rotation run outside the lock — no DjVuLibre calls */
  if (surface && need_alpha)
    djvu_fill_alpha (surface);

  FW_TRACE_DJVU ("render_page done: page=%d %dx%d surface=%p",
                  page, w, h, (void *) surface);
  return djvu_apply_rotation (surface, w, h, rotation);
}

/* ── TOC extraction ───────────────────────────────────────────────── */

static FwTocNode *
miniexp_to_toc (ddjvu_document_t *doc, miniexp_t exp)
{
  if (!miniexp_consp (exp))
    return NULL;

  FwTocNode *first = NULL;
  FwTocNode *prev  = NULL;

  for (miniexp_t item = exp; miniexp_consp (item); item = miniexp_cdr (item)) {
    miniexp_t entry = miniexp_car (item);
    if (!miniexp_consp (entry))
      continue;

    /* Entry format: (title url child...) */
    miniexp_t title_exp = miniexp_car (entry);
    if (!miniexp_stringp (title_exp))
      continue;

    const char *title = miniexp_to_str (title_exp);
    int dest_page = -1;

    miniexp_t url_exp = miniexp_cadr (entry);
    if (miniexp_stringp (url_exp)) {
      const char *url = miniexp_to_str (url_exp);
      /* DjVu internal links are "#pageN" */
      if (url[0] == '#') {
        dest_page = atoi (url + 1) - 1;  /* 1-based → 0-based */
      }
    }

    FwTocNode *node = fw_toc_node_new (title, dest_page);

    /* Recurse into children (cddr of the entry) */
    miniexp_t children = miniexp_cddr (entry);
    node->children = miniexp_to_toc (doc, children);

    if (prev)
      prev->next = node;
    else
      first = node;
    prev = node;
  }

  return first;
}

static FwTocNode *
djvu_get_toc (FwDocument *doc)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);

  miniexp_t outline = ddjvu_document_get_outline (self->djvu_doc);
  if (outline == miniexp_dummy)
    return NULL;

  FwTocNode *result = NULL;

  if (miniexp_consp (outline)) {
    /* Top-level is (bookmarks entry...) — skip the 'bookmarks' symbol */
    miniexp_t entries = miniexp_cdr (outline);
    result = miniexp_to_toc (self->djvu_doc, entries);
  }

  return result;
}

/* ── Search — DjVuLibre text layer search ─────────────────────────── */

static void
collect_text_from_sexpr (miniexp_t exp, GString *buf)
{
  if (miniexp_stringp (exp)) {
    g_string_append (buf, miniexp_to_str (exp));
    return;
  }

  if (!miniexp_consp (exp))
    return;

  /* S-expression: (type x0 y0 x1 y1 content...) */
  miniexp_t rest = exp;
  for (int i = 0; i < 5 && miniexp_consp (rest); i++)
    rest = miniexp_cdr (rest);

  for (; miniexp_consp (rest); rest = miniexp_cdr (rest))
    collect_text_from_sexpr (miniexp_car (rest), buf);
}

/* Collect word text whose bbox overlaps [qx0,qy0,qx1,qy1], expressed in
 * DjVu hidden-text pixel space (image pixels, bottom-left origin). Each
 * node is (type x0 y0 x1 y1 content...); leaf word nodes carry a string
 * child. `all` collects everything regardless of the query (whole-page
 * fallback). Words are space-separated so a selection copies as readable
 * text. */
static void
collect_text_in_rect (miniexp_t exp, GString *buf,
                      int qx0, int qy0, int qx1, int qy1, gboolean all)
{
  if (!miniexp_consp (exp))
    return;

  miniexp_t p = miniexp_cdr (exp);   /* skip the type symbol */
  int box[4] = { 0, 0, 0, 0 };
  for (int i = 0; i < 4 && miniexp_consp (p); i++) {
    miniexp_t c = miniexp_car (p);
    if (miniexp_numberp (c)) box[i] = miniexp_to_int (c);
    p = miniexp_cdr (p);
  }
  gboolean overlaps = all ||
    !(box[2] < qx0 || box[0] > qx1 || box[3] < qy0 || box[1] > qy1);

  for (; miniexp_consp (p); p = miniexp_cdr (p)) {
    miniexp_t child = miniexp_car (p);
    if (miniexp_stringp (child)) {
      if (overlaps) {
        if (buf->len > 0) g_string_append_c (buf, ' ');
        g_string_append (buf, miniexp_to_str (child));
      }
    } else {
      collect_text_in_rect (child, buf, qx0, qy0, qx1, qy1, all);
    }
  }
}

static GArray *
djvu_search (FwDocument *doc, const char *text, int page)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);
  GArray *hits = g_array_new (FALSE, FALSE, sizeof (FwSearchHit));

  /* Cancel beat — return whatever we have if a search was cancelled.
   * Without this, fw-search's per-page worker has no way to bail
   * mid-document on a slow DjVu file (single-mutex backend, slow
   * first-page text decode). The caller's main-thread join then
   * stalls the UI. Sample once at entry so a cancel during this
   * page's decode short-circuits before doing the work. */
  gint start_gen = g_atomic_int_get (&self->cancel_gen);

  miniexp_t page_text = ddjvu_document_get_pagetext (self->djvu_doc,
                                                      page, "word");
  if (page_text == miniexp_dummy)
    return hits;
  if (g_atomic_int_get (&self->cancel_gen) != start_gen)
    return hits;

  /* Walk the s-expression tree to find words matching the search text.
   * This is a simple substring search at the word level. */
  /* For now, extract full page text and do simple search — proper
   * rectangle-based hit reporting requires walking the tree with coords. */
  GString *buf = g_string_new (NULL);
  collect_text_from_sexpr (page_text, buf);

  /* Simple case-insensitive search for hit count */
  const char *haystack = buf->str;
  const char *needle = text;
  size_t needle_len = strlen (needle);

  char *haystack_lower = g_utf8_strdown (haystack, -1);
  char *needle_lower = g_utf8_strdown (needle, -1);
  const char *p = haystack_lower;
  while ((p = strstr (p, needle_lower)) != NULL) {
    /* Without proper coordinate mapping, report page-level hit */
    ddjvu_pageinfo_t info;
    ddjvu_document_get_pageinfo (self->djvu_doc, page, &info);
    double dpi = (double) info.dpi;
    if (dpi <= 0) dpi = 300.0;

    FwSearchHit hit = {
      .page = page,
      .x0 = 0, .y0 = 0,
      .x1 = (double) info.width * 72.0 / dpi,
      .y1 = (double) info.height * 72.0 / dpi,
    };
    g_array_append_val (hits, hit);
    p += needle_len;
  }

  g_free (haystack_lower);
  g_free (needle_lower);
  g_string_free (buf, TRUE);
  return hits;
}

/* ── Text extraction ──────────────────────────────────────────────── */

static char *
djvu_get_text (FwDocument *doc, int page,
               double x0, double y0, double x1, double y1)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);

  miniexp_t page_text = ddjvu_document_get_pagetext (self->djvu_doc,
                                                      page, "word");
  if (page_text == miniexp_dummy)
    return NULL;

  /* Map the doc-space query rect (points, top-left origin) into DjVu
   * hidden-text pixel space (image pixels, bottom-left origin), then
   * collect only the words it overlaps. A degenerate rect — or missing
   * page info — collects the whole page (the previous behaviour). DjVu's
   * bottom-left convention means the y axis is flipped against the
   * page height. (Selection precision here is worth an interactive
   * confirm; the coordinate convention is DjVu's documented one.) */
  gboolean all = (x1 <= x0 || y1 <= y0);
  int qx0 = 0, qy0 = 0, qx1 = 0, qy1 = 0;
  if (!all) {
    ddjvu_pageinfo_t info;
    if (ddjvu_document_get_pageinfo (self->djvu_doc, page, &info) == DDJVU_JOB_OK
        && info.dpi > 0 && info.height > 0) {
      double scale = (double) info.dpi / 72.0;   /* points → pixels */
      qx0 = (int) (x0 * scale);
      qx1 = (int) (x1 * scale);
      qy0 = (int) (info.height - y1 * scale);
      qy1 = (int) (info.height - y0 * scale);
    } else {
      all = TRUE;
    }
  }

  GString *buf = g_string_new (NULL);
  collect_text_in_rect (page_text, buf, qx0, qy0, qx1, qy1, all);

  if (buf->len == 0) {
    g_string_free (buf, TRUE);
    return NULL;
  }

  return g_string_free (buf, FALSE);
}

/* ── Links — DjVu files rarely have links ─────────────────────────── */

static GArray *
djvu_get_links (FwDocument *doc, int page)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);
  GArray *links = g_array_new (FALSE, FALSE, sizeof (FwLink *));
  g_array_set_clear_func (links, (GDestroyNotify) fw_link_free_indirect);

  miniexp_t annotations = ddjvu_document_get_pageanno (self->djvu_doc, page);
  if (annotations == miniexp_dummy)
    return links;

  /* DjVu page annotations can contain maparea entries with links.
   * Walk the s-expression for (maparea url comment area) entries. */
  for (miniexp_t item = annotations; miniexp_consp (item);
       item = miniexp_cdr (item)) {
    miniexp_t entry = miniexp_car (item);
    if (!miniexp_consp (entry))
      continue;

    miniexp_t sym = miniexp_car (entry);
    if (!miniexp_symbolp (sym))
      continue;

    if (strcmp (miniexp_to_name (sym), "maparea") != 0)
      continue;

    /* (maparea url comment area) */
    miniexp_t url_exp = miniexp_cadr (entry);
    if (!miniexp_stringp (url_exp))
      continue;

    const char *url = miniexp_to_str (url_exp);
    miniexp_t area_exp = miniexp_nth (3, entry);
    if (!miniexp_consp (area_exp))
      continue;

    /* area: (rect x y w h) */
    miniexp_t area_type = miniexp_car (area_exp);
    if (!miniexp_symbolp (area_type) ||
        strcmp (miniexp_to_name (area_type), "rect") != 0)
      continue;

    int coords[4];
    miniexp_t rest = miniexp_cdr (area_exp);
    for (int i = 0; i < 4 && miniexp_consp (rest); i++) {
      coords[i] = miniexp_to_int (miniexp_car (rest));
      rest = miniexp_cdr (rest);
    }

    ddjvu_pageinfo_t info;
    ddjvu_document_get_pageinfo (self->djvu_doc, page, &info);
    double dpi = (double) info.dpi;
    if (dpi <= 0) dpi = 300.0;
    double scale = 72.0 / dpi;

    double lx0 = coords[0] * scale;
    double ly0 = coords[1] * scale;
    double lx1 = (coords[0] + coords[2]) * scale;
    double ly1 = (coords[1] + coords[3]) * scale;

    FwLink *link;
    if (url[0] == '#') {
      int dest = atoi (url + 1) - 1;
      link = fw_link_new_internal (lx0, ly0, lx1, ly1, dest);
    } else {
      link = fw_link_new_external (lx0, ly0, lx1, ly1, url);
    }
    g_array_append_val (links, link);
  }

  return links;
}

/* ── Page handle API ─────────────────────────────────────────────── */

static gpointer
djvu_open_page (FwDocument *doc, int page)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);
  ddjvu_page_t *pg = NULL;

  FW_TRACE_DJVU ("open_page: page=%d", page);
  g_mutex_lock (&self->render_lock);

  pg = ddjvu_page_create_by_pageno (self->djvu_doc, page);
  if (pg) {
    djvu_wait_for_job (self->djvu_ctx, ddjvu_page_job (pg));
    djvu_drain_messages (self->djvu_ctx);

    if (ddjvu_page_decoding_error (pg)) {
      g_warning ("DjVu: decoding error on page %d", page);
      ddjvu_page_release (pg);
      pg = NULL;
      FW_TRACE_DJVU ("open_page FAILED (decode error): page=%d", page);
    }
  }

  g_mutex_unlock (&self->render_lock);
  FW_TRACE_DJVU ("open_page done: page=%d handle=%p", page, (void *) pg);
  return pg;
}

static void
djvu_close_page (FwDocument *doc, gpointer handle)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);
  ddjvu_page_t *pg = handle;

  FW_TRACE_DJVU ("close_page: handle=%p", (void *) pg);
  if (pg) {
    /* Must hold render_lock — ddjvu_page_release touches context state
     * that is not thread-safe. */
    g_mutex_lock (&self->render_lock);
    ddjvu_page_release (pg);
    g_mutex_unlock (&self->render_lock);
  }
}

static cairo_surface_t *
djvu_render_page_from_handle (FwDocument *doc, gpointer handle,
                              double zoom, int rotation)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);
  ddjvu_page_t *pg = handle;
  cairo_surface_t *surface = NULL;

  if (!pg)
    return NULL;

  FW_TRACE_DJVU ("render_from_handle start: handle=%p zoom=%.2f rot=%d",
                  (void *) pg, zoom, rotation);
  gint start_gen = g_atomic_int_get (&self->cancel_gen);
  g_mutex_lock (&self->render_lock);

  if (g_atomic_int_get (&self->cancel_gen) != start_gen) {
    g_mutex_unlock (&self->render_lock);
    FW_TRACE_DJVU ("render_from_handle cancelled: handle=%p", (void *) pg);
    return NULL;
  }

  int dpi = ddjvu_page_get_resolution (pg);
  if (dpi <= 0) dpi = 300;

  int orig_w = ddjvu_page_get_width (pg);
  int orig_h = ddjvu_page_get_height (pg);

  double scale = zoom * 72.0 / (double) dpi;
  int w = (int) ((double) orig_w * scale + 0.5);
  int h = (int) ((double) orig_h * scale + 0.5);

  if (w < 1) w = 1;
  if (h < 1) h = 1;

  gboolean need_alpha = FALSE;
  surface = djvu_render_to_cairo (self->render_fmt, pg, w, h, &need_alpha);
  g_mutex_unlock (&self->render_lock);

  /* Alpha fill and rotation run outside the lock */
  if (surface && need_alpha)
    djvu_fill_alpha (surface);

  FW_TRACE_DJVU ("render_from_handle done: handle=%p %dx%d surface=%p",
                  (void *) pg, w, h, (void *) surface);
  return djvu_apply_rotation (surface, w, h, rotation);
}

static void
djvu_cancel_render (FwDocument *doc)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (doc);
  FW_TRACE_DJVU ("cancel_render%s", "");
  g_atomic_int_inc (&self->cancel_gen);
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_document_djvu_dispose (GObject *object)
{
  djvu_close (FW_DOCUMENT (object));
  G_OBJECT_CLASS (fw_document_djvu_parent_class)->dispose (object);
}

static void
fw_document_djvu_finalize (GObject *object)
{
  FwDocumentDjvu *self = FW_DOCUMENT_DJVU (object);
  g_mutex_clear (&self->render_lock);
  G_OBJECT_CLASS (fw_document_djvu_parent_class)->finalize (object);
}

static void
fw_document_djvu_class_init (FwDocumentDjvuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose  = fw_document_djvu_dispose;
  object_class->finalize = fw_document_djvu_finalize;
}

static void
fw_document_djvu_init (FwDocumentDjvu *self)
{
  g_mutex_init (&self->render_lock);
}

static void
fw_document_djvu_iface_init (FwDocumentInterface *iface)
{
  iface->open           = djvu_open;
  iface->close          = djvu_close;
  iface->get_page_count = djvu_get_page_count;
  iface->get_page_size  = djvu_get_page_size;
  iface->render_page    = djvu_render_page;
  iface->get_toc        = djvu_get_toc;
  iface->search         = djvu_search;
  iface->get_text       = djvu_get_text;
  iface->get_links      = djvu_get_links;
  iface->open_page      = djvu_open_page;
  iface->close_page     = djvu_close_page;
  iface->render_page_from_handle = djvu_render_page_from_handle;
  iface->cancel_render  = djvu_cancel_render;
}

FwDocumentDjvu *
fw_document_djvu_new (void)
{
  return g_object_new (FW_TYPE_DOCUMENT_DJVU, NULL);
}
