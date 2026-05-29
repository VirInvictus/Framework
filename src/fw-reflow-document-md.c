/* fw-reflow-document-md.c — Markdown reflow backend
 *
 * Renders Markdown through the WebView path: md4c (vendored, GitHub
 * dialect) converts the document to HTML, which produce_html wraps with
 * the shared reading stylesheet and hands to the WebView. Raw inline
 * HTML is disabled (MD_FLAG_NOHTML) so an untrusted .md can't inject
 * <script> into the JS-enabled WebView; literal HTML is escaped instead.
 *
 * This backend has no block model (the legacy FwReflowView path is on
 * its way out); it only implements produce_html plus the metadata/TOC
 * accessors. get_block_model returns an empty store to satisfy the
 * interface.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document-md.h"
#include "fw-reflow-html.h"

#include <md4c-html.h>

struct _FwReflowDocumentMd {
  GObject       parent_instance;
  char         *md;          /* raw Markdown, UTF-8 (owned) */
  gsize         md_len;
  GHashTable   *metadata;    /* gchar* → gchar* */
  GListStore   *blocks;      /* empty — satisfies the interface */
  GListStore   *toc;         /* empty */
  char         *path;
};

static void fw_reflow_document_md_iface_init (FwReflowDocumentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwReflowDocumentMd,
                               fw_reflow_document_md,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (FW_TYPE_REFLOW_DOCUMENT,
                                                      fw_reflow_document_md_iface_init))

/* ── Interface implementation ─────────────────────────────────────── */

static gboolean
md_open (FwReflowDocument *doc, const char *path, GError **error)
{
  FwReflowDocumentMd *self = FW_REFLOW_DOCUMENT_MD (doc);

  g_autofree char *raw = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &raw, &len, error))
    return FALSE;

  /* Markdown is UTF-8 by convention; fall back to Latin-1 (never fails)
   * for stray byte sequences rather than refusing the file. */
  if (g_utf8_validate_len (raw, len, NULL)) {
    self->md     = g_strndup (raw, len);
    self->md_len = len;
  } else {
    g_autoptr (GError) e = NULL;
    self->md = g_convert (raw, len, "UTF-8", "ISO-8859-1", NULL,
                          &self->md_len, &e);
    if (!self->md) {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "Could not decode '%s' as text", path);
      return FALSE;
    }
  }

  if (!self->metadata)
    self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, g_free);
  g_autofree char *basename = g_path_get_basename (path);
  g_hash_table_insert (self->metadata, g_strdup ("title"), g_strdup (basename));
  g_hash_table_insert (self->metadata, g_strdup ("format"), g_strdup ("Markdown"));

  self->path = g_strdup (path);
  return TRUE;
}

static void
md_close (FwReflowDocument *doc)
{
  (void) doc;
}

static GListModel *
md_get_block_model (FwReflowDocument *doc)
{
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_MD (doc)->blocks);
}

static GdkTexture *
md_get_image (FwReflowDocument *doc, const char *image_id)
{
  (void) doc; (void) image_id;
  return NULL;
}

static GListModel *
md_get_toc (FwReflowDocument *doc)
{
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_MD (doc)->toc);
}

static guint
md_find_block_by_anchor (FwReflowDocument *doc, const char *anchor_id)
{
  (void) doc; (void) anchor_id;
  return 0;
}

static GHashTable *
md_get_metadata (FwReflowDocument *doc)
{
  FwReflowDocumentMd *self = FW_REFLOW_DOCUMENT_MD (doc);
  return self->metadata ? g_hash_table_ref (self->metadata) : NULL;
}

static void
md_html_sink (const MD_CHAR *text, MD_SIZE size, void *userdata)
{
  g_string_append_len ((GString *) userdata, text, (gssize) size);
}

static gboolean
md_produce_html (FwReflowDocument *doc, const char *doc_id,
                 char **out_html, GHashTable **out_images, GError **error)
{
  (void) doc_id;
  FwReflowDocumentMd *self = FW_REFLOW_DOCUMENT_MD (doc);

  g_autoptr (GString) body = g_string_new (NULL);
  int rc = md_html (self->md, (MD_SIZE) self->md_len,
                    md_html_sink, body,
                    MD_DIALECT_GITHUB | MD_FLAG_NOHTML,
                    0);
  if (rc != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "md4c failed to render Markdown");
    return FALSE;
  }

  const char *title = self->metadata
                        ? g_hash_table_lookup (self->metadata, "title") : NULL;

  GString *out = g_string_new ("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">");
  if (title && *title) {
    g_autofree char *e = g_markup_escape_text (title, -1);
    g_string_append_printf (out, "<title>%s</title>", e);
  }
  g_string_append (out, "<style>");
  g_string_append (out, fw_reflow_reading_css ());
  g_string_append (out, "</style></head><body><section data-spine=\"0\">");
  g_string_append_len (out, body->str, (gssize) body->len);
  g_string_append (out, "</section></body></html>");

  if (out_images) *out_images = NULL;
  *out_html = g_string_free (out, FALSE);
  return TRUE;
}

static void
fw_reflow_document_md_iface_init (FwReflowDocumentInterface *iface)
{
  iface->open                  = md_open;
  iface->close                 = md_close;
  iface->get_block_model       = md_get_block_model;
  iface->get_image             = md_get_image;
  iface->get_toc               = md_get_toc;
  iface->find_block_by_anchor  = md_find_block_by_anchor;
  iface->get_metadata          = md_get_metadata;
  iface->produce_html          = md_produce_html;
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_reflow_document_md_finalize (GObject *object)
{
  FwReflowDocumentMd *self = FW_REFLOW_DOCUMENT_MD (object);
  g_clear_pointer (&self->md, g_free);
  g_clear_pointer (&self->metadata, g_hash_table_unref);
  g_clear_object (&self->blocks);
  g_clear_object (&self->toc);
  g_clear_pointer (&self->path, g_free);
  G_OBJECT_CLASS (fw_reflow_document_md_parent_class)->finalize (object);
}

static void
fw_reflow_document_md_class_init (FwReflowDocumentMdClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fw_reflow_document_md_finalize;
}

static void
fw_reflow_document_md_init (FwReflowDocumentMd *self)
{
  self->blocks = g_list_store_new (FW_TYPE_BLOCK);
  self->toc    = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
}

FwReflowDocumentMd *
fw_reflow_document_md_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_DOCUMENT_MD, NULL);
}
