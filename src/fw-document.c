/* fw-document.c — Abstract document interface implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-document-pdf.h"
#include "fw-document-djvu.h"

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

/* ── GInterface boilerplate ───────────────────────────────────────── */

G_DEFINE_INTERFACE (FwDocument, fw_document, G_TYPE_OBJECT)

static void
fw_document_default_init (FwDocumentInterface *iface)
{
  (void) iface;
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

  if (g_ascii_strcasecmp (dot, ".pdf") == 0) {
    doc = FW_DOCUMENT (fw_document_pdf_new ());
  } else if (g_ascii_strcasecmp (dot, ".djvu") == 0 ||
             g_ascii_strcasecmp (dot, ".djv") == 0) {
    doc = FW_DOCUMENT (fw_document_djvu_new ());
  } else {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "Unsupported file type: %s", dot);
    return NULL;
  }

  if (!fw_document_open (doc, path, error)) {
    g_object_unref (doc);
    return NULL;
  }

  return doc;
}
