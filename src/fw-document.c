/* fw-document.c — Abstract document interface implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-document-pdf.h"
#include "fw-document-djvu.h"
#include "fw-document-cbr.h"
#include "fw-debug.h"

#include <gio/gio.h>
#include <string.h>

/* ── TOC node ─────────────────────────────────────────────────────── */

FwTocNode *
fw_toc_node_new (const char *title, int page)
{
  FwTocNode *node = g_new0 (FwTocNode, 1);
  node->title = g_strdup (title);
  node->page  = page;
  return node;
}

void
fw_toc_node_free (FwTocNode *node)
{
  if (!node)
    return;

  fw_toc_node_free (node->children);
  fw_toc_node_free (node->next);
  g_free (node->title);
  g_free (node);
}

/* ── Link helpers ─────────────────────────────────────────────────── */

FwLink *
fw_link_new_internal (double x0, double y0, double x1, double y1,
                      int dest_page)
{
  FwLink *link = g_new0 (FwLink, 1);
  link->type = FW_LINK_INTERNAL;
  link->x0 = x0;  link->y0 = y0;
  link->x1 = x1;  link->y1 = y1;
  link->dest_page = dest_page;
  return link;
}

FwLink *
fw_link_new_external (double x0, double y0, double x1, double y1,
                      const char *uri)
{
  FwLink *link = g_new0 (FwLink, 1);
  link->type = FW_LINK_EXTERNAL;
  link->x0 = x0;  link->y0 = y0;
  link->x1 = x1;  link->y1 = y1;
  link->uri = g_strdup (uri);
  return link;
}

void
fw_link_free (FwLink *link)
{
  if (!link)
    return;
  g_free (link->uri);
  g_free (link);
}

/* GArray clear function for arrays of FwLink*.
 * g_array_set_clear_func passes a pointer TO the element — since elements
 * are FwLink*, the callback receives FwLink**. */
void
fw_link_free_indirect (gpointer data)
{
  FwLink **pp = data;
  fw_link_free (*pp);
}

/* ── Embedded file attachment ────────────────────────────────────── */

void
fw_attachment_free (FwAttachment *a)
{
  if (!a)
    return;
  g_free (a->name);
  g_free (a->mime_type);
  /* backend_data is opaque to the interface — backends register a custom
   * cleanup via direct call paths if needed. */
  g_free (a);
}

void
fw_attachment_free_indirect (gpointer data)
{
  FwAttachment **pp = data;
  fw_attachment_free (*pp);
}

/* ── GInterface boilerplate ───────────────────────────────────────── */

G_DEFINE_INTERFACE (FwDocument, fw_document, G_TYPE_OBJECT)

static guint signals[1];  /* GEOMETRY_CHANGED */

static void
fw_document_default_init (FwDocumentInterface *iface)
{
  (void) iface;

  /* Emitted when a backend refines its page geometry after open — e.g.
   * the CBR backend, which defaults every page to the cover's size at
   * open and only learns real per-page dimensions from a background
   * probe. Consumers (the view / window) relayout and re-apply
   * fit-width. Backends with accurate sizes at open (PDF/DjVu/MuPDF)
   * never emit it. */
  signals[0] = g_signal_new ("geometry-changed",
                             FW_TYPE_DOCUMENT,
                             G_SIGNAL_RUN_LAST,
                             0, NULL, NULL, NULL,
                             G_TYPE_NONE, 0);
}

void
fw_document_emit_geometry_changed (FwDocument *self)
{
  g_return_if_fail (FW_IS_DOCUMENT (self));
  g_signal_emit (self, signals[0], 0);
}

/* ── Public API — delegates to vtable ─────────────────────────────── */

gboolean
fw_document_open (FwDocument *self, const char *path, GError **error)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), FALSE);
  return FW_DOCUMENT_GET_IFACE (self)->open (self, path, error);
}

void
fw_document_close (FwDocument *self)
{
  g_return_if_fail (FW_IS_DOCUMENT (self));
  FW_DOCUMENT_GET_IFACE (self)->close (self);
}

int
fw_document_get_page_count (FwDocument *self)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), 0);
  return FW_DOCUMENT_GET_IFACE (self)->get_page_count (self);
}

void
fw_document_get_page_size (FwDocument *self, int page,
                           double *width, double *height)
{
  g_return_if_fail (FW_IS_DOCUMENT (self));
  FW_DOCUMENT_GET_IFACE (self)->get_page_size (self, page, width, height);
}

cairo_surface_t *
fw_document_render_page (FwDocument *self, int page,
                         double zoom, int rotation)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  return FW_DOCUMENT_GET_IFACE (self)->render_page (self, page, zoom, rotation);
}

FwTocNode *
fw_document_get_toc (FwDocument *self)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  return FW_DOCUMENT_GET_IFACE (self)->get_toc (self);
}

GArray *
fw_document_search (FwDocument *self, const char *text, int page)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  return FW_DOCUMENT_GET_IFACE (self)->search (self, text, page);
}

char *
fw_document_get_text (FwDocument *self, int page,
                      double x0, double y0, double x1, double y1)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  return FW_DOCUMENT_GET_IFACE (self)->get_text (self, page, x0, y0, x1, y1);
}

GArray *
fw_document_get_links (FwDocument *self, int page)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  return FW_DOCUMENT_GET_IFACE (self)->get_links (self, page);
}

/* ── Page handle API ─────────────────────────────────────────────── */

gpointer
fw_document_open_page (FwDocument *self, int page)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->open_page)
    return iface->open_page (self, page);
  return NULL;
}

void
fw_document_close_page (FwDocument *self, gpointer handle)
{
  g_return_if_fail (FW_IS_DOCUMENT (self));
  if (!handle) return;
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->close_page)
    iface->close_page (self, handle);
}

cairo_surface_t *
fw_document_render_page_from_handle (FwDocument *self, gpointer handle,
                                     double zoom, int rotation)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->render_page_from_handle && handle)
    return iface->render_page_from_handle (self, handle, zoom, rotation);
  /* Fallback: not available */
  return NULL;
}

void
fw_document_cancel_render (FwDocument *self)
{
  g_return_if_fail (FW_IS_DOCUMENT (self));
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->cancel_render)
    iface->cancel_render (self);
}

GArray *
fw_document_get_attachments (FwDocument *self)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->get_attachments)
    return iface->get_attachments (self);
  return NULL;
}

gboolean
fw_document_save_attachment (FwDocument *self, FwAttachment *attachment,
                             const char *output_path, GError **error)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), FALSE);
  g_return_val_if_fail (attachment != NULL, FALSE);
  g_return_val_if_fail (output_path != NULL, FALSE);

  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (!iface->save_attachment) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                         "Backend does not support attachment extraction");
    return FALSE;
  }
  return iface->save_attachment (self, attachment, output_path, error);
}

GHashTable *
fw_document_get_metadata (FwDocument *self)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->get_metadata)
    return iface->get_metadata (self);
  return NULL;
}

gboolean
fw_document_select_at (FwDocument *self, int page, double x, double y,
                       FwSelectGranularity gran,
                       double *out_x0, double *out_y0,
                       double *out_x1, double *out_y1)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), FALSE);
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->select_at)
    return iface->select_at (self, page, x, y, gran,
                             out_x0, out_y0, out_x1, out_y1);
  return FALSE;
}

GArray *
fw_document_get_selection_quads (FwDocument *self, int page,
                                 double x0, double y0,
                                 double x1, double y1)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), NULL);
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->get_selection_quads)
    return iface->get_selection_quads (self, page, x0, y0, x1, y1);
  return NULL;
}

gboolean
fw_document_get_content_bbox (FwDocument *self, int page,
                              double *out_x0, double *out_y0,
                              double *out_x1, double *out_y1)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), FALSE);
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->get_content_bbox)
    return iface->get_content_bbox (self, page, out_x0, out_y0, out_x1, out_y1);
  return FALSE;
}

gboolean
fw_document_is_spread_filename (FwDocument *self, int page)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (self), FALSE);
  FwDocumentInterface *iface = FW_DOCUMENT_GET_IFACE (self);
  if (iface->is_spread_filename)
    return iface->is_spread_filename (self, page);
  return FALSE;
}

/* ── Factory — pick backend by file extension ─────────────────────── */

FwDocument *
fw_document_new_for_path (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, NULL);

  const char *dot = strrchr (path, '.');
  if (!dot) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "Cannot determine file type: no extension");
    return NULL;
  }

  FwDocument *doc = NULL;
  const char *backend_label;

  /* MuPDF dispatches PDF, CBZ/CB7/CBT, EPUB, FB2, MOBI, XPS via
   * fz_register_document_handlers + fz_open_document. The PDF backend's
   * open() path is format-agnostic — anything MuPDF recognizes opens here.
   * MuPDF gives ZIP/7z/tar comics fast random page access; only RAR
   * (no central directory) needs the sequential libarchive backend. DjVu
   * remains separate because DjVuLibre is its own library. */
  if (g_ascii_strcasecmp (dot, ".pdf") == 0) {
    doc = FW_DOCUMENT (fw_document_pdf_new ());
    backend_label = "PDF (MuPDF)";
  } else if (g_ascii_strcasecmp (dot, ".cbz") == 0 ||
             g_ascii_strcasecmp (dot, ".cb7") == 0 ||
             g_ascii_strcasecmp (dot, ".cbt") == 0) {
    doc = FW_DOCUMENT (fw_document_pdf_new ());
    backend_label = "Comic (MuPDF)";
  } else if (g_ascii_strcasecmp (dot, ".cbr") == 0) {
    doc = FW_DOCUMENT (fw_document_cbr_new ());
    backend_label = "Comic-RAR (libarchive)";
  } else if (g_ascii_strcasecmp (dot, ".xps") == 0 ||
             g_ascii_strcasecmp (dot, ".oxps") == 0) {
    doc = FW_DOCUMENT (fw_document_pdf_new ());
    backend_label = "XPS (MuPDF)";
  } else if (g_ascii_strcasecmp (dot, ".epub") == 0 ||
             g_ascii_strcasecmp (dot, ".fb2") == 0 ||
             g_ascii_strcasecmp (dot, ".mobi") == 0 ||
             g_ascii_strcasecmp (dot, ".azw") == 0 ||
             g_ascii_strcasecmp (dot, ".azw3") == 0 ||
             g_ascii_strcasecmp (dot, ".prc") == 0) {
    doc = FW_DOCUMENT (fw_document_pdf_new ());
    backend_label = "Reflowable (MuPDF)";
  } else if (g_ascii_strcasecmp (dot, ".djvu") == 0 ||
             g_ascii_strcasecmp (dot, ".djv") == 0) {
    doc = FW_DOCUMENT (fw_document_djvu_new ());
    backend_label = "DjVu";
  } else {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "Unsupported file type: %s", dot);
    return NULL;
  }

  FW_TRACE_DOC ("opening '%s' with %s backend", path, backend_label);

  if (!fw_document_open (doc, path, error)) {
    /* Callers may legally pass error == NULL; don't deref it for the
     * trace line. */
    if (error && *error)
      FW_TRACE_DOC ("open FAILED: %s", (*error)->message);
    else
      FW_TRACE_DOC ("open FAILED (no error set)");
    g_object_unref (doc);
    return NULL;
  }

  FW_TRACE_DOC ("opened '%s': %d pages", path,
                fw_document_get_page_count (doc));
  return doc;
}
