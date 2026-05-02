/* fw-reflow-document.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document.h"
#include "fw-reflow-document-txt.h"
#include "fw-reflow-document-fb2.h"
#include "fw-reflow-document-epub.h"
#include "fw-reflow-document-mobi.h"

#include <string.h>

/* ── FwBlock GObject ──────────────────────────────────────────────── */

struct _FwBlock {
  GObject       parent_instance;
  FwBlockKind   kind;
  int           level;
  char         *text;
  char         *image_id;
  char         *anchor_id;
  guint         flags;
};

G_DEFINE_FINAL_TYPE (FwBlock, fw_block, G_TYPE_OBJECT)

static void
fw_block_finalize (GObject *object)
{
  FwBlock *self = FW_BLOCK (object);
  g_free (self->text);
  g_free (self->image_id);
  g_free (self->anchor_id);
  G_OBJECT_CLASS (fw_block_parent_class)->finalize (object);
}

static void
fw_block_class_init (FwBlockClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fw_block_finalize;
}

static void
fw_block_init (FwBlock *self)
{
  (void) self;
}

FwBlock *
fw_block_new (FwBlockKind kind, int level, const char *text,
              const char *image_id, const char *anchor_id, guint flags)
{
  FwBlock *self = g_object_new (FW_TYPE_BLOCK, NULL);
  self->kind      = kind;
  self->level     = level;
  self->text      = text      ? g_strdup (text)      : NULL;
  self->image_id  = image_id  ? g_strdup (image_id)  : NULL;
  self->anchor_id = anchor_id ? g_strdup (anchor_id) : NULL;
  self->flags     = flags;
  return self;
}

FwBlockKind  fw_block_get_kind       (FwBlock *self) { return self->kind;      }
int          fw_block_get_level      (FwBlock *self) { return self->level;     }
const char  *fw_block_get_text       (FwBlock *self) { return self->text;      }
const char  *fw_block_get_image_id   (FwBlock *self) { return self->image_id;  }
const char  *fw_block_get_anchor_id  (FwBlock *self) { return self->anchor_id; }
guint        fw_block_get_flags      (FwBlock *self) { return self->flags;     }

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
fw_reflow_document_get_block_model (FwReflowDocument *self)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), NULL);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->get_block_model ? iface->get_block_model (self) : NULL;
}

GdkTexture *
fw_reflow_document_get_image (FwReflowDocument *self, const char *image_id)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), NULL);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->get_image ? iface->get_image (self, image_id) : NULL;
}

GListModel *
fw_reflow_document_get_toc (FwReflowDocument *self)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), NULL);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->get_toc ? iface->get_toc (self) : NULL;
}

guint
fw_reflow_document_find_block_by_anchor (FwReflowDocument *self, const char *anchor_id)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), 0);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->find_block_by_anchor ? iface->find_block_by_anchor (self, anchor_id) : 0;
}

GArray *
fw_reflow_document_search (FwReflowDocument *self, const char *needle)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), NULL);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->search ? iface->search (self, needle) : NULL;
}

GHashTable *
fw_reflow_document_get_metadata (FwReflowDocument *self)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), NULL);
  FwReflowDocumentInterface *iface = FW_REFLOW_DOCUMENT_GET_IFACE (self);
  return iface->get_metadata ? iface->get_metadata (self) : NULL;
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

gboolean
fw_reflow_path_is_supported (const char *path)
{
  /* Reflow-handled formats (foliate-js ports):
   *   TXT  (v0.40.0)
   *   FB2  (v0.41.0)
   *   EPUB (v0.42.0)
   *   MOBI / PRC — KF7 only (v0.52.0)
   *
   * AZW3 / .azw / KF8 — handled by MuPDF's reflowable backend
   * until full KF8 (foliate's INDX + SKEL + FRAG splice) lands. */
  return path_has_ext (path, "txt") ||
         path_has_ext (path, "fb2") ||
         path_has_ext (path, "epub") ||
         path_has_ext (path, "mobi") ||
         path_has_ext (path, "prc");
}

FwReflowDocument *
fw_reflow_document_new_for_path (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, NULL);

  FwReflowDocument *doc = NULL;

  if (path_has_ext (path, "txt"))
    doc = FW_REFLOW_DOCUMENT (fw_reflow_document_txt_new ());
  else if (path_has_ext (path, "fb2"))
    doc = FW_REFLOW_DOCUMENT (fw_reflow_document_fb2_new ());
  else if (path_has_ext (path, "epub"))
    doc = FW_REFLOW_DOCUMENT (fw_reflow_document_epub_new ());
  else if (path_has_ext (path, "mobi") || path_has_ext (path, "prc"))
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
