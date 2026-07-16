/* fw-reflow-document.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document.h"
#include "fw-reflow-document-txt.h"
#include "fw-reflow-document-fb2.h"
#include "fw-reflow-document-epub.h"
#include "fw-reflow-document-mobi.h"
#include "fw-reflow-document-md.h"

#include <string.h>

/* ── FwReflowTocItem GObject ──────────────────────────────────────── */

struct _FwReflowTocItem {
  GObject  parent_instance;
  char    *title;
  char    *anchor_id;
};

G_DEFINE_FINAL_TYPE (FwReflowTocItem, fw_reflow_toc_item, G_TYPE_OBJECT)

static void
fw_reflow_toc_item_finalize (GObject *object)
{
  FwReflowTocItem *self = FW_REFLOW_TOC_ITEM (object);
  g_free (self->title);
  g_free (self->anchor_id);
  G_OBJECT_CLASS (fw_reflow_toc_item_parent_class)->finalize (object);
}

static void
fw_reflow_toc_item_class_init (FwReflowTocItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fw_reflow_toc_item_finalize;
}

static void
fw_reflow_toc_item_init (FwReflowTocItem *self)
{
  (void) self;
}

FwReflowTocItem *
fw_reflow_toc_item_new (const char *title, const char *anchor_id)
{
  FwReflowTocItem *self = g_object_new (FW_TYPE_REFLOW_TOC_ITEM, NULL);
  self->title     = title     ? g_strdup (title)     : NULL;
  self->anchor_id = anchor_id ? g_strdup (anchor_id) : NULL;
  return self;
}

const char *
fw_reflow_toc_item_get_title (FwReflowTocItem *self)
{
  return self ? self->title : NULL;
}

const char *
fw_reflow_toc_item_get_anchor_id (FwReflowTocItem *self)
{
  return self ? self->anchor_id : NULL;
}

/* ── FwReflowDocument interface ───────────────────────────────────── */

G_DEFINE_INTERFACE (FwReflowDocument, fw_reflow_document, G_TYPE_OBJECT)

static void
fw_reflow_document_default_init (FwReflowDocumentInterface *iface)
{
  (void) iface;
}

gboolean
fw_reflow_document_open (FwReflowDocument *self, const char *path, GError **error)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), FALSE);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->open ? iface->open (self, path, error) : FALSE;
}

void
fw_reflow_document_close (FwReflowDocument *self)
{
  g_return_if_fail (FW_IS_REFLOW_DOCUMENT (self));
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  if (iface->close)
    iface->close (self);
}



GListModel *
fw_reflow_document_get_toc (FwReflowDocument *self)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), NULL);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->get_toc ? iface->get_toc (self) : NULL;
}


GHashTable *
fw_reflow_document_get_metadata (FwReflowDocument *self)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), NULL);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->get_metadata ? iface->get_metadata (self) : NULL;
}

gboolean
fw_reflow_document_supports_html (FwReflowDocument *self)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), FALSE);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->produce_html != NULL;
}

gboolean
fw_reflow_document_produce_html (FwReflowDocument  *self,
                                 const char        *doc_id,
                                 char             **out_html,
                                 GHashTable       **out_images,
                                 GError           **error)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), FALSE);
  g_return_val_if_fail (doc_id != NULL, FALSE);
  g_return_val_if_fail (out_html != NULL, FALSE);

  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  if (!iface->produce_html) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "reflow backend does not implement produce_html");
    return FALSE;
  }
  return iface->produce_html (self, doc_id, out_html, out_images, error);
}

GHashTable *
fw_reflow_document_get_resources (FwReflowDocument *self)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), NULL);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->get_resources ? iface->get_resources (self) : NULL;
}

/* ── Path probe + factory ─────────────────────────────────────────── */

static gboolean
path_has_ext (const char *path, const char *ext_lower)
{
  if (!path)
    return FALSE;
  const char *dot = strrchr (path, '.');
  if (!dot)
    return FALSE;
  return g_ascii_strcasecmp (dot + 1, ext_lower) == 0;
}

/* Case-insensitive suffix match — for compound extensions like .fb2.zip
 * that path_has_ext (last-component only) can't catch. */
static gboolean
path_has_suffix_ci (const char *path, const char *suffix)
{
  if (!path)
    return FALSE;
  gsize lp = strlen (path), ls = strlen (suffix);
  return lp >= ls && g_ascii_strcasecmp (path + lp - ls, suffix) == 0;
}

gboolean
fw_reflow_path_is_supported (const char *path)
{
  /* Reflow-handled formats (foliate-js ports):
   *   TXT  (v0.40.0)
   *   FB2 / FB2.ZIP — bare (v0.41.0), zipped (v0.67.0)
   *   EPUB (v0.42.0)
   *   MOBI / PRC — KF7 (v0.52.0) and KF8 (v0.55.0)
   *   AZW / AZW3 — KF8 (v0.55.0) */
  return path_has_ext (path, "txt") ||
         path_has_ext (path, "md") ||
         path_has_ext (path, "markdown") ||
         path_has_ext (path, "fb2") ||
         path_has_suffix_ci (path, ".fb2.zip") ||
         path_has_ext (path, "epub") ||
         path_has_ext (path, "mobi") ||
         path_has_ext (path, "prc") ||
         path_has_ext (path, "azw") ||
         path_has_ext (path, "azw3");
}

FwReflowDocument *
fw_reflow_document_new_for_path (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, NULL);

  FwReflowDocument *doc = NULL;

  if (path_has_ext (path, "txt"))
    doc = FW_REFLOW_DOCUMENT (fw_reflow_document_txt_new ());
  else if (path_has_ext (path, "md") || path_has_ext (path, "markdown"))
    doc = FW_REFLOW_DOCUMENT (fw_reflow_document_md_new ());
  else if (path_has_ext (path, "fb2") || path_has_suffix_ci (path, ".fb2.zip"))
    doc = FW_REFLOW_DOCUMENT (fw_reflow_document_fb2_new ());
  else if (path_has_ext (path, "epub"))
    doc = FW_REFLOW_DOCUMENT (fw_reflow_document_epub_new ());
  else if (path_has_ext (path, "mobi") || path_has_ext (path, "prc") ||
           path_has_ext (path, "azw")  || path_has_ext (path, "azw3"))
    doc = FW_REFLOW_DOCUMENT (fw_reflow_document_mobi_new ());

  if (!doc) {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                 "No reflow backend for: %s", path);
    return NULL;
  }

  if (!fw_reflow_document_open (doc, path, error)) {
    g_object_unref (doc);
    return NULL;
  }

  return doc;
}
