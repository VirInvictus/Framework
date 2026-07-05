/* fw-reflow-document-txt.c — TXT reflow backend
 *
 * produce_html splits the file on blank-line boundaries to emit one
 * <p> per chunk for the WebView. Encoding is UTF-8 with a fallback to
 * ISO-8859-1 (and a BOM-driven UTF-16 unwrap) when bytes don't validate.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document-txt.h"
#include "fw-reflow-html.h"

struct _FwReflowDocumentTxt {
  GObject      parent_instance;
  char        *text;       /* decoded UTF-8 body (owned) */
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

/* ── Paragraph emitter ────────────────────────────────────────────── */

/* Emit one <p> for the [start, end) chunk after trimming surrounding
 * whitespace and HTML-escaping. Intra-paragraph newlines become <br>.
 * Empty trimmed text is dropped. */
static void
emit_paragraph (GString *out, const char *start, const char *end)
{
  while (start < end && g_ascii_isspace (*start))
    start++;
  while (end > start && g_ascii_isspace (*(end - 1)))
    end--;
  if (start >= end)
    return;

  g_autofree char *raw = g_strndup (start, end - start);
  g_autofree char *escaped = g_markup_escape_text (raw, -1);

  g_string_append (out, "<p>");
  for (const char *p = escaped; *p; p++) {
    if (*p == '\n') g_string_append (out, "<br>");
    else            g_string_append_c (out, *p);
  }
  g_string_append (out, "</p>");
}

/* Walk the decoded text, splitting on runs of two or more newlines
 * (allowing optional CRs) — Markdown-style blank-line separation — and
 * emit a <p> per paragraph into `out`. */
static void
emit_paragraphs (GString *out, const char *utf8)
{
  if (!utf8 || !*utf8)
    return;

  const char *p = utf8;
  const char *block_start = p;

  while (*p) {
    if (*p == '\n') {
      const char *nl = p;
      const char *q = p + 1;
      gboolean blank_run = FALSE;
      while (*q == '\r' || *q == ' ' || *q == '\t') q++;
      if (*q == '\n') {
        blank_run = TRUE;
        while (*q == '\n' || *q == '\r' || *q == ' ' || *q == '\t')
          q++;
      }

      if (blank_run) {
        emit_paragraph (out, block_start, nl);
        block_start = q;
        p = q;
        continue;
      }
    }
    p++;
  }

  /* Trailing paragraph (no trailing blank line). */
  emit_paragraph (out, block_start, p);
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

  self->text = g_steal_pointer (&utf8);
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
  (void) doc;
}

static GListModel *
txt_get_toc (FwReflowDocument *doc)
{
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (doc);
  return G_LIST_MODEL (self->toc);
}

static GHashTable *
txt_get_metadata (FwReflowDocument *doc)
{
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (doc);
  return self->metadata ? g_hash_table_ref (self->metadata) : NULL;
}

/* WebView path: split the decoded text on blank lines and emit a <p>
 * per paragraph (escaped, intra-paragraph newlines as <br>). No images. */
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

  emit_paragraphs (out, self->text);

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
  iface->get_toc               = txt_get_toc;
  iface->get_metadata          = txt_get_metadata;
  iface->produce_html          = txt_produce_html;
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_reflow_document_txt_finalize (GObject *object)
{
  FwReflowDocumentTxt *self = FW_REFLOW_DOCUMENT_TXT (object);
  g_clear_pointer (&self->text, g_free);
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
  self->toc = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
}

FwReflowDocumentTxt *
fw_reflow_document_txt_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_DOCUMENT_TXT, NULL);
}
