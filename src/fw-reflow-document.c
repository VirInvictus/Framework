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
void         fw_block_set_flags      (FwBlock *self, guint flags) { self->flags = flags; }

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

/* ── Search ───────────────────────────────────────────────────────── */

/* CODE and CHAPTER blocks render via gtk_label_set_text (no markup);
 * every other kind goes through gtk_label_set_markup. The search /
 * highlight machinery has to treat those two as raw text. */
static gboolean
block_text_is_markup (FwBlockKind kind)
{
  return kind != FW_BLOCK_CODE && kind != FW_BLOCK_CHAPTER;
}

/* Decode an XML/Pango entity beginning at `p` (which must point at '&').
 * Writes the decoded UTF-8 into `out` and returns its byte length, or 0
 * when `p` is not a well-formed entity (the caller then treats '&' as a
 * literal byte). `*src_len` receives the source bytes consumed (& … ;).
 * Handles the named entities GLib/Pango emit (amp/lt/gt/quot/apos) plus
 * decimal and hex numeric references — notably &#39; for the apostrophes
 * that fill book prose. */
static int
decode_entity (const char *p, char out[8], int *src_len)
{
  const char *sc = strchr (p, ';');
  if (!sc || sc - p > 12 || sc == p + 1)
    return 0;
  int n = (int) (sc - p - 1);
  const char *body = p + 1;
  gunichar cp = 0;

  if (body[0] == '#') {
    if (n >= 2 && (body[1] == 'x' || body[1] == 'X'))
      cp = (gunichar) g_ascii_strtoull (body + 2, NULL, 16);
    else if (n >= 1)
      cp = (gunichar) g_ascii_strtoull (body + 1, NULL, 10);
  } else if (n == 3 && strncmp (body, "amp",  3) == 0) cp = '&';
  else if   (n == 2 && strncmp (body, "lt",   2) == 0) cp = '<';
  else if   (n == 2 && strncmp (body, "gt",   2) == 0) cp = '>';
  else if   (n == 4 && strncmp (body, "quot", 4) == 0) cp = '"';
  else if   (n == 4 && strncmp (body, "apos", 4) == 0) cp = '\'';

  if (cp == 0 || !g_unichar_validate (cp))
    return 0;
  *src_len = (int) (sc - p) + 1;
  return g_unichar_to_utf8 (cp, out);
}

/* Visible text of a block: markup kinds get <tags> stripped and
 * &entities; decoded; plain kinds (code / chapter) pass through. The
 * byte layout produced here is the coordinate space FwReflowHit offsets
 * index into, and MUST match the walk in fw-reflow-view.c's highlight
 * splicer or highlights land on the wrong characters. */
static char *
fw_reflow_block_visible_text (FwBlock *block)
{
  g_return_val_if_fail (FW_IS_BLOCK (block), g_strdup (""));
  const char *t = fw_block_get_text (block);
  if (!t)
    return g_strdup ("");
  if (!block_text_is_markup (fw_block_get_kind (block)))
    return g_strdup (t);

  GString *out = g_string_new (NULL);
  for (const char *p = t; *p; ) {
    if (*p == '<') {
      const char *gt = strchr (p, '>');
      if (!gt) break;
      p = gt + 1;
      continue;
    }
    if (*p == '&') {
      char dec[8];
      int src_len = 0;
      int dl = decode_entity (p, dec, &src_len);
      if (dl > 0) {
        g_string_append_len (out, dec, dl);
        p += src_len;
        continue;
      }
    }
    const char *next = g_utf8_next_char (p);
    if (next <= p) next = p + 1;       /* defensive against bad UTF-8 */
    g_string_append_len (out, p, next - p);
    p = next;
  }
  return g_string_free (out, FALSE);
}

GArray *
fw_reflow_document_search (FwReflowDocument *self, const char *needle)
{
  g_return_val_if_fail (FW_IS_REFLOW_DOCUMENT (self), NULL);
  if (!needle || !needle[0])
    return NULL;

  GListModel *model = fw_reflow_document_get_block_model (self);
  if (!model)
    return NULL;

  /* Needle as lowercased codepoints. Comparing codepoint-by-codepoint
   * (rather than g_utf8_strdown + strstr like the fixed-layout backends)
   * keeps match offsets aligned to the *original* visible bytes, which
   * downcasing can otherwise shift — and the splicer needs original
   * offsets. User-visible behaviour (case-insensitive substring) is the
   * same. */
  glong ncps = 0;
  gunichar *needle_cp = g_utf8_to_ucs4_fast (needle, -1, &ncps);
  if (!needle_cp || ncps == 0) {
    g_free (needle_cp);
    return NULL;
  }
  for (glong k = 0; k < ncps; k++)
    needle_cp[k] = g_unichar_tolower (needle_cp[k]);

  GArray *hits = g_array_new (FALSE, FALSE, sizeof (FwReflowHit));
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++) {
    g_autoptr (FwBlock) block = g_list_model_get_item (model, i);
    if (!block)
      continue;
    g_autofree char *vis = fw_reflow_block_visible_text (block);
    if (!vis || !vis[0])
      continue;

    for (const char *p = vis; *p; ) {
      const char *q = p;
      gboolean ok = TRUE;
      for (glong k = 0; k < ncps; k++) {
        if (!*q) { ok = FALSE; break; }
        if (g_unichar_tolower (g_utf8_get_char (q)) != needle_cp[k]) {
          ok = FALSE;
          break;
        }
        q = g_utf8_next_char (q);
      }
      if (ok) {
        FwReflowHit hit = { .block  = i,
                            .offset = (guint) (p - vis),
                            .length = (guint) (q - p) };
        g_array_append_val (hits, hit);
        p = q;                 /* non-overlapping matches */
      } else {
        p = g_utf8_next_char (p);
      }
    }
  }
  g_free (needle_cp);

  if (hits->len == 0) {
    g_array_unref (hits);
    return NULL;
  }
  return hits;
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
