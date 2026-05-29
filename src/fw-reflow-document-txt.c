/* fw-reflow-document-txt.c — TXT reflow backend
 *
 * Splits the file on blank-line boundaries to produce one
 * FW_BLOCK_PARAGRAPH per chunk. Encoding is UTF-8 with a fallback to
 * ISO-8859-1 (and a BOM-driven UTF-16 unwrap) when bytes don't validate.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document-txt.h"
#include "fw-reflow-html.h"

#include <string.h>

struct _FwReflowDocumentTxt {
  GObject      parent_instance;
  GListStore  *blocks;     /* GListStore<FwBlock> */
  GListStore  *toc;        /* GListStore<FwReflowTocItem> — empty for TXT */
  GHashTable  *metadata;   /* gchar* → gchar* */
  char        *path;
};

static void fw_reflow_document_txt_iface_init (FwReflowDocumentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwReflowDocumentTxt,
                               fw_reflow_document_txt,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (FW_TYPE_REFLOW_DOCUMENT,
                                                      fw_reflow_document_txt_iface_init))

/* ── Encoding normalization ───────────────────────────────────────── */

/* Normalize a freshly-read byte buffer to a NUL-terminated UTF-8 string.
 * Detects UTF-16 BOM; otherwise tries UTF-8; falls back to Latin-1 (which
 * never fails on arbitrary bytes). Returns NULL on unrecoverable errors. */
static char *
bytes_to_utf8 (const char *data, gsize len)
{
  if (len >= 3 && (guchar)data[0] == 0xEF && (guchar)data[1] == 0xBB && (guchar)data[2] == 0xBF) {
    /* UTF-8 BOM — strip and use the rest. */
    return g_strndup (data + 3, len - 3);
  }

  if (len >= 2 && (guchar)data[0] == 0xFF && (guchar)data[1] == 0xFE) {
    /* UTF-16LE BOM */
    g_autoptr (GError) e = NULL;
    char *out = g_convert (data + 2, len - 2, "UTF-8", "UTF-16LE",
                           NULL, NULL, &e);
    if (out)
      return out;
  }

  if (len >= 2 && (guchar)data[0] == 0xFE && (guchar)data[1] == 0xFF) {
    /* UTF-16BE BOM */
    g_autoptr (GError) e = NULL;
    char *out = g_convert (data + 2, len - 2, "UTF-8", "UTF-16BE",
                           NULL, NULL, &e);
    if (out)
      return out;
  }

  if (g_utf8_validate_len (data, len, NULL))
    return g_strndup (data, len);

  /* Last-ditch: Latin-1 always succeeds. */
  g_autoptr (GError) e = NULL;
  char *out = g_convert (data, len, "UTF-8", "ISO-8859-1", NULL, NULL, &e);
  return out;
}

/* ── Paragraph splitter ───────────────────────────────────────────── */

/* Push a paragraph after trimming surrounding blank lines and escaping
 * Pango markup. Empty trimmed text is dropped. */
static void
push_paragraph (GListStore *store, const char *start, const char *end)
{
  /* Trim leading whitespace */
  while (start < end && g_ascii_isspace (*start))
    start++;
  /* Trim trailing whitespace */
  while (end > start && g_ascii_isspace (*(end - 1)))
    end--;
  if (start >= end)
    return;

  g_autofree char *raw = g_strndup (start, end - start);
  g_autofree char *escaped = g_markup_escape_text (raw, -1);

  FwBlock *block = fw_block_new (FW_BLOCK_PARAGRAPH, 0, escaped,
                                 NULL, NULL, 0);
  g_list_store_append (store, block);
  g_object_unref (block);
}

static void
build_blocks_from_text (GListStore *store, const char *utf8)
{
  if (!utf8 || !*utf8)
    return;

  /* Split on runs of two or more newlines (allowing optional CRs), so
   * paragraph boundaries match the way TXT authors actually write
   * — Markdown-style blank-line separation. */
  const char *p = utf8;
  const char *block_start = p;

  while (*p) {
    if (*p == '\n') {
      const char *nl = p;
      const char *q = p + 1;
      /* Skip optional CRs and whitespace until we either find a second
       * newline (paragraph break) or hit a non-blank character. */
      gboolean blank_run = FALSE;
      while (*q == '\r' || *q == ' ' || *q == '\t') q++;
      if (*q == '\n') {
        blank_run = TRUE;
        /* Consume the entire run of blank-only lines. */
        while (*q == '\n' || *q == '\r' || *q == ' ' || *q == '\t')
          q++;
      }

      if (blank_run) {
        push_paragraph (store, block_start, nl);
        block_start = q;
        p = q;
        continue;
      }
    }
    p++;
  }

  /* Trailing paragraph (no trailing blank line). */
  push_paragraph (store, block_start, p);
}

/* ── Interface implementation ─────────────────────────────────────── */

static gboolean
txt_open (FwReflowDocument *doc, const char *path, GError **error)
{
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (doc);

  g_autofree char *raw = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &raw, &len, error))
    return FALSE;

  g_autofree char *utf8 = bytes_to_utf8 (raw, len);
  if (!utf8) {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                 "Could not decode '%s' as text", path);
    return FALSE;
  }

  build_blocks_from_text (self->blocks, utf8);
  self->path = g_strdup (path);

  /* Minimal metadata — filename + byte size. */
  if (!self->metadata)
    self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, g_free);
  g_autofree char *basename = g_path_get_basename (path);
  g_hash_table_insert (self->metadata, g_strdup ("title"), g_strdup (basename));
  g_hash_table_insert (self->metadata, g_strdup ("format"), g_strdup ("Plain Text"));

  return TRUE;
}

static void
txt_close (FwReflowDocument *doc)
{
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (doc);
  if (self->blocks)
    g_list_store_remove_all (self->blocks);
}

static GListModel *
txt_get_block_model (FwReflowDocument *doc)
{
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (doc);
  return G_LIST_MODEL (self->blocks);
}

static GdkTexture *
txt_get_image (FwReflowDocument *doc, const char *image_id)
{
  (void) doc; (void) image_id;
  return NULL;  /* TXT has no images. */
}

static GListModel *
txt_get_toc (FwReflowDocument *doc)
{
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (doc);
  return G_LIST_MODEL (self->toc);
}

static guint
txt_find_block_by_anchor (FwReflowDocument *doc, const char *anchor_id)
{
  (void) doc; (void) anchor_id;
  return 0;
}

static GHashTable *
txt_get_metadata (FwReflowDocument *doc)
{
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (doc);
  return self->metadata ? g_hash_table_ref (self->metadata) : NULL;
}

/* WebView path: wrap each parsed paragraph in <p> (the block text is
 * already g_markup_escape_text'd, so it's HTML-safe), with intra-paragraph
 * newlines as <br>. No images. */
static gboolean
txt_produce_html (FwReflowDocument *doc, const char *doc_id,
                  char **out_html, GHashTable **out_images, GError **error)
{
  (void) doc_id; (void) error;
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (doc);

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

  GListModel *m = G_LIST_MODEL (self->blocks);
  guint n = g_list_model_get_n_items (m);
  for (guint i = 0; i < n; i++) {
    g_autoptr (FwBlock) b = g_list_model_get_item (m, i);
    const char *t = fw_block_get_text (b);
    if (!t || !*t) continue;
    g_string_append (out, "<p>");
    for (const char *p = t; *p; p++) {
      if (*p == '\n') g_string_append (out, "<br>");
      else            g_string_append_c (out, *p);
    }
    g_string_append (out, "</p>");
  }

  g_string_append (out, "</section></body></html>");
  if (out_images) *out_images = NULL;
  *out_html = g_string_free (out, FALSE);
  return TRUE;
}

static void
fw_reflow_document_txt_iface_init (FwReflowDocumentInterface *iface)
{
  iface->open                  = txt_open;
  iface->close                 = txt_close;
  iface->get_block_model       = txt_get_block_model;
  iface->get_image             = txt_get_image;
  iface->get_toc               = txt_get_toc;
  iface->find_block_by_anchor  = txt_find_block_by_anchor;
  iface->get_metadata          = txt_get_metadata;
  iface->produce_html          = txt_produce_html;
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_reflow_document_txt_finalize (GObject *object)
{
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (object);
  g_clear_object (&self->blocks);
  g_clear_object (&self->toc);
  g_clear_pointer (&self->metadata, g_hash_table_unref);
  g_clear_pointer (&self->path, g_free);
  G_OBJECT_CLASS (fw_reflow_document_txt_parent_class)->finalize (object);
}

static void
fw_reflow_document_txt_class_init (FwReflowDocumentTxtClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fw_reflow_document_txt_finalize;
}

static void
fw_reflow_document_txt_init (FwReflowDocumentTxt *self)
{
  self->blocks = g_list_store_new (FW_TYPE_BLOCK);
  self->toc    = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
}

FwReflowDocumentTxt *
fw_reflow_document_txt_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_DOCUMENT_TXT, NULL);
}
