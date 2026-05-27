/* fw-reflow-document-fb2.c — FB2 (FictionBook 2) reflow backend
 *
 * libxml2-based port from `.foliate-js/fb2.js`. The FB2 transform
 * tables in foliate (STYLE / SECTION / POEM / TABLE / BODY) define
 * how each FB2 element maps to XHTML; we go directly from FB2 XML
 * to FwBlocks via tree walk, applying the same logical mapping:
 *
 *   <section>            → CHAPTER marker; recurse with depth+1
 *   <title> in <section> → HEADING block at level=section depth
 *   <p>                  → PARAGRAPH block
 *   <subtitle>           → HEADING block at level=section depth + 1
 *   <epigraph>, <cite>,
 *   <poem>, <annotation> → BLOCKQUOTE block
 *   <empty-line/>        → HR block
 *   <image l:href="#X">  → IMAGE block (resolved against <binary id=X>)
 *
 * Inline tags ported via the same Pango-markup translation EPUB and
 * MOBI use; an inline-stack auto-closes on block-boundary so spans
 * crossing paragraphs don't break Pango parsing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document-fb2.h"

#include <string.h>
#include <archive.h>
#include <archive_entry.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

struct _FwReflowDocumentFb2 {
  GObject       parent_instance;
  GListStore   *blocks;     /* GListStore<FwBlock> */
  GListStore   *toc;        /* GListStore<FwReflowTocItem> */
  GHashTable   *metadata;   /* gchar* → gchar* */
  GHashTable   *images;     /* gchar* (id) → GdkTexture* (owned) */
  GHashTable   *anchors;    /* gchar* (anchor_id) → guint position+1 */
  char         *path;
};

static void fw_reflow_document_fb2_iface_init (FwReflowDocumentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwReflowDocumentFb2,
                               fw_reflow_document_fb2,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (FW_TYPE_REFLOW_DOCUMENT,
                                                      fw_reflow_document_fb2_iface_init))

/* ── Walker context ──────────────────────────────────────────────── */

typedef struct {
  FwReflowDocumentFb2 *doc;

  int           section_depth;     /* 0 outside body */

  /* Block accumulator (active between p/title open and close). */
  gboolean      accum_active;
  FwBlockKind   accum_kind;
  int           accum_level;
  GString      *accum;
  char         *pending_anchor;

  /* Inline-tag stack — same purpose as in the EPUB / MOBI walkers. */
  GPtrArray    *open_inlines;

  /* TOC: when inside a section's <title>, accumulate plain text. */
  GString      *toc_title;
  char         *toc_anchor;

  /* Metadata accumulators. */
  GString      *author_first;
  GString      *author_middle;
  GString      *author_last;
} Fb2WalkCtx;

/* ── XLink helper — read href in any namespace prefix form ───── */

static const char *
fb2_get_href (xmlNode *n)
{
  /* FB2 uses xlink:href; some files declare it as `l:href`. Match
   * whichever is present. */
  for (xmlAttr *a = n->properties; a; a = a->next) {
    const char *name = (const char *)a->name;
    if ((g_str_equal (name, "href") || g_ascii_strcasecmp (name, "href") == 0)
        && a->children)
      return (const char *)a->children->content;
  }
  return NULL;
}

/* ── Inline-tag mapping (foliate's STYLE table) ───────────────── */

static const char *
fb2_pango_for_inline (const char *lname)
{
  if (g_str_equal (lname, "emphasis"))                          return "i";
  if (g_str_equal (lname, "strong"))                            return "b";
  if (g_str_equal (lname, "code"))                              return "tt";
  if (g_str_equal (lname, "sub"))                               return "sub";
  if (g_str_equal (lname, "sup"))                               return "sup";
  if (g_str_equal (lname, "strikethrough"))                     return "s";
  if (g_str_equal (lname, "a"))                                 return "u";
  /* `style` and `span` map to no Pango span; pass children through. */
  return NULL;
}

/* ── Accumulator helpers ──────────────────────────────────────── */

static void
fb2_flush (Fb2WalkCtx *cc)
{
  if (!cc->accum_active) return;
  cc->accum_active = FALSE;

  if (cc->open_inlines) {
    for (gint i = (gint) cc->open_inlines->len - 1; i >= 0; i--)
      g_string_append_printf (cc->accum, "</%s>",
                              (const char *) cc->open_inlines->pdata[i]);
  }

  while (cc->accum->len > 0 &&
         g_ascii_isspace (cc->accum->str[cc->accum->len - 1]))
    g_string_truncate (cc->accum, cc->accum->len - 1);

  if (cc->accum->len > 0) {
    FwBlock *b = fw_block_new (cc->accum_kind, cc->accum_level,
                               cc->accum->str, NULL,
                               cc->pending_anchor, 0);
    g_list_store_append (cc->doc->blocks, b);
    if (cc->pending_anchor) {
      guint pos = g_list_model_get_n_items (G_LIST_MODEL (cc->doc->blocks)) - 1;
      g_hash_table_insert (cc->doc->anchors,
                           g_strdup (cc->pending_anchor),
                           GUINT_TO_POINTER (pos + 1));
    }
    g_object_unref (b);
  }
  g_string_truncate (cc->accum, 0);
  g_clear_pointer (&cc->pending_anchor, g_free);
  cc->accum_level = 0;
}

static void
fb2_re_emit_inlines (Fb2WalkCtx *cc)
{
  if (!cc->open_inlines) return;
  for (guint i = 0; i < cc->open_inlines->len; i++)
    g_string_append_printf (cc->accum, "<%s>",
                            (const char *) cc->open_inlines->pdata[i]);
}

static void
fb2_start_block (Fb2WalkCtx *cc, FwBlockKind kind, int level, xmlNode *element)
{
  fb2_flush (cc);
  cc->accum_active = TRUE;
  cc->accum_kind   = kind;
  cc->accum_level  = level;
  for (xmlAttr *a = element->properties; a; a = a->next) {
    if (g_ascii_strcasecmp ((const char *)a->name, "id") == 0 && a->children) {
      const char *val = (const char *)a->children->content;
      if (val && *val) {
        g_clear_pointer (&cc->pending_anchor, g_free);
        cc->pending_anchor = g_strdup (val);
      }
      break;
    }
  }
  fb2_re_emit_inlines (cc);
}

static void
fb2_text_escaped (GString *out, const char *text, gboolean to_toc, GString *toc_dst)
{
  if (!text) return;
  for (const char *p = text; *p; p++) {
    char c = *p;
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && out->len > 0 && out->str[out->len - 1] == ' ')
      continue;
    if      (c == '<') g_string_append (out, "&lt;");
    else if (c == '>') g_string_append (out, "&gt;");
    else if (c == '&') g_string_append (out, "&amp;");
    else               g_string_append_c (out, c);

    if (to_toc && toc_dst) g_string_append_c (toc_dst, *p);
  }
}

/* ── Forward decls for mutual recursion ──────────────────────── */

static void fb2_walk_node     (Fb2WalkCtx *cc, xmlNode *node);
static void fb2_walk_metadata (Fb2WalkCtx *cc, xmlNode *node);
static void fb2_walk_binaries (Fb2WalkCtx *cc, xmlNode *node);
static void fb2_metadata_set  (FwReflowDocumentFb2 *self,
                               const char *key, const char *val);

/* ── Body walker ──────────────────────────────────────────────── */

static void
fb2_handle_inline (Fb2WalkCtx *cc, xmlNode *n, const char *lname)
{
  const char *pname = fb2_pango_for_inline (lname);
  if (pname && cc->accum_active) {
    g_string_append_printf (cc->accum, "<%s>", pname);
    g_ptr_array_add (cc->open_inlines, (gpointer) pname);
    fb2_walk_node (cc, n->children);
    if (cc->open_inlines->len > 0 &&
        cc->open_inlines->pdata[cc->open_inlines->len - 1] == pname) {
      g_ptr_array_remove_index (cc->open_inlines,
                                 cc->open_inlines->len - 1);
      if (cc->accum_active)
        g_string_append_printf (cc->accum, "</%s>", pname);
    }
  } else {
    fb2_walk_node (cc, n->children);
  }
}

static void
fb2_handle_image (Fb2WalkCtx *cc, xmlNode *n)
{
  fb2_flush (cc);
  const char *href = fb2_get_href (n);
  if (!href) return;
  /* Strip leading '#'. */
  const char *id = href[0] == '#' ? href + 1 : href;
  FwBlock *img = fw_block_new (FW_BLOCK_IMAGE, 0, NULL, id, NULL, 0);
  g_list_store_append (cc->doc->blocks, img);
  g_object_unref (img);
}

static void
fb2_handle_section (Fb2WalkCtx *cc, xmlNode *n)
{
  fb2_flush (cc);
  cc->section_depth++;

  /* Record the section's id so the upcoming <title>'s flushed
   * HEADING block carries it as its anchor. */
  const char *section_id = NULL;
  for (xmlAttr *a = n->properties; a; a = a->next) {
    if (g_ascii_strcasecmp ((const char *)a->name, "id") == 0 && a->children) {
      section_id = (const char *)a->children->content;
      break;
    }
  }

  /* CHAPTER marker. */
  FwBlock *chap = fw_block_new (FW_BLOCK_CHAPTER, cc->section_depth,
                                NULL, NULL, section_id, 0);
  g_list_store_append (cc->doc->blocks, chap);
  if (section_id) {
    guint pos = g_list_model_get_n_items (G_LIST_MODEL (cc->doc->blocks)) - 1;
    g_hash_table_insert (cc->doc->anchors,
                         g_strdup (section_id),
                         GUINT_TO_POINTER (pos + 1));
  }
  g_object_unref (chap);

  /* Stash for the title TOC entry. */
  g_clear_pointer (&cc->toc_anchor, g_free);
  if (section_id) cc->toc_anchor = g_strdup (section_id);

  fb2_walk_node (cc, n->children);

  fb2_flush (cc);
  if (cc->section_depth > 0)
    cc->section_depth--;
}

static void
fb2_handle_title (Fb2WalkCtx *cc, xmlNode *n)
{
  if (cc->section_depth <= 0) {
    /* `<title>` at the top of <body> — the book title. Use it as a
     * metadata-title fallback: fb2_metadata_set is first-write-wins and
     * the metadata pre-pass (canonical <title-info><book-title>) runs
     * first, so this only fills in when a malformed FB2 omits title-info.
     * Children are still walked so the title also renders as content. */
    xmlChar *content = xmlNodeGetContent (n);
    if (content) {
      g_autofree char *t = g_strstrip (g_strdup ((const char *) content));
      if (*t) fb2_metadata_set (cc->doc, "title", t);
      xmlFree (content);
    }
    fb2_walk_node (cc, n->children);
    return;
  }

  fb2_flush (cc);
  cc->accum_active = TRUE;
  cc->accum_kind   = FW_BLOCK_HEADING;
  cc->accum_level  = cc->section_depth;
  /* Anchor = the section's id (collected at fb2_handle_section). */
  g_clear_pointer (&cc->pending_anchor, g_free);
  if (cc->toc_anchor) {
    cc->pending_anchor = cc->toc_anchor;
    cc->toc_anchor = NULL;     /* transfer ownership */
  }
  if (!cc->toc_title) cc->toc_title = g_string_new (NULL);
  else                g_string_truncate (cc->toc_title, 0);

  /* `<title>` wraps `<p>` elements; treat each `<p>` as a line of
   * the heading rather than a new block. To do this, we walk the
   * children but avoid starting new accumulators. */
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type == XML_TEXT_NODE && c->content) {
      fb2_text_escaped (cc->accum, (const char *)c->content,
                        TRUE, cc->toc_title);
    } else if (c->type == XML_ELEMENT_NODE) {
      g_autofree char *cname = g_ascii_strdown ((const char *)c->name, -1);
      if (g_str_equal (cname, "p")) {
        if (cc->accum->len > 0) g_string_append_c (cc->accum, ' ');
        fb2_walk_node (cc, c->children);
      } else if (g_str_equal (cname, "empty-line")) {
        g_string_append (cc->accum, "\n");
      } else {
        fb2_walk_node (cc, c);
      }
    }
  }

  /* Push TOC entry from the plain-text mirror. */
  if (cc->toc_title && cc->toc_title->len > 0 && cc->pending_anchor) {
    while (cc->toc_title->len > 0 &&
           g_ascii_isspace (cc->toc_title->str[cc->toc_title->len - 1]))
      g_string_truncate (cc->toc_title, cc->toc_title->len - 1);
    FwReflowTocItem *item =
      fw_reflow_toc_item_new (cc->toc_title->str, cc->pending_anchor);
    g_list_store_append (cc->doc->toc, item);
    g_object_unref (item);
  }

  fb2_flush (cc);
}

static void
fb2_handle_block_quote (Fb2WalkCtx *cc, xmlNode *n)
{
  /* `epigraph` / `cite` / `poem` / `annotation` — walk children
   * with PARAGRAPH-promoted-to-BLOCKQUOTE. We change accum_kind
   * for any <p> we encounter inside. */
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    g_autofree char *cname = g_ascii_strdown ((const char *)c->name, -1);
    if (g_str_equal (cname, "p") || g_str_equal (cname, "v") ||
        g_str_equal (cname, "subtitle") || g_str_equal (cname, "text-author")) {
      fb2_start_block (cc, FW_BLOCK_BLOCKQUOTE, 0, c);
      fb2_walk_node (cc, c->children);
      fb2_flush (cc);
    } else if (g_str_equal (cname, "empty-line")) {
      fb2_flush (cc);
      FwBlock *hr = fw_block_new (FW_BLOCK_HR, 0, NULL, NULL, NULL, 0);
      g_list_store_append (cc->doc->blocks, hr);
      g_object_unref (hr);
    } else if (g_str_equal (cname, "stanza")) {
      /* Stanza: nested children of <stanza> behave like p's. */
      fb2_handle_block_quote (cc, c);
    } else {
      fb2_walk_node (cc, c->children);
    }
  }
}

static void
fb2_handle_element (Fb2WalkCtx *cc, xmlNode *n)
{
  const char *name = (const char *)n->name;
  g_autofree char *lname = g_ascii_strdown (name, -1);

  if (g_str_equal (lname, "section")) {
    fb2_handle_section (cc, n);
    return;
  }
  if (g_str_equal (lname, "title")) {
    fb2_handle_title (cc, n);
    return;
  }
  if (g_str_equal (lname, "subtitle")) {
    fb2_start_block (cc, FW_BLOCK_HEADING,
                     cc->section_depth + 1, n);
    fb2_walk_node (cc, n->children);
    fb2_flush (cc);
    return;
  }
  if (g_str_equal (lname, "p")) {
    fb2_start_block (cc, FW_BLOCK_PARAGRAPH, 0, n);
    fb2_walk_node (cc, n->children);
    fb2_flush (cc);
    return;
  }
  if (g_str_equal (lname, "empty-line")) {
    fb2_flush (cc);
    FwBlock *hr = fw_block_new (FW_BLOCK_HR, 0, NULL, NULL, NULL, 0);
    g_list_store_append (cc->doc->blocks, hr);
    g_object_unref (hr);
    return;
  }
  if (g_str_equal (lname, "epigraph") || g_str_equal (lname, "cite") ||
      g_str_equal (lname, "poem") || g_str_equal (lname, "annotation")) {
    fb2_handle_block_quote (cc, n);
    return;
  }
  if (g_str_equal (lname, "image")) {
    fb2_handle_image (cc, n);
    return;
  }
  if (g_str_equal (lname, "v")) {
    /* Verse line — paragraph with line-break feel. */
    fb2_start_block (cc, FW_BLOCK_PARAGRAPH, 0, n);
    fb2_walk_node (cc, n->children);
    fb2_flush (cc);
    return;
  }

  /* Inline tags inside an active accumulator. */
  fb2_handle_inline (cc, n, lname);
}

static void
fb2_walk_node (Fb2WalkCtx *cc, xmlNode *node)
{
  for (xmlNode *n = node; n; n = n->next) {
    if (n->type == XML_ELEMENT_NODE) {
      fb2_handle_element (cc, n);
    } else if (n->type == XML_TEXT_NODE && cc->accum_active) {
      fb2_text_escaped (cc->accum, (const char *)n->content,
                        FALSE, NULL);
    }
  }
}

/* ── Metadata walker ─────────────────────────────────────────── */

static void
fb2_metadata_set (FwReflowDocumentFb2 *self, const char *key, const char *val)
{
  if (!val || !*val) return;
  if (g_hash_table_contains (self->metadata, key)) return;
  g_hash_table_insert (self->metadata, g_strdup (key), g_strdup (val));
}

static char *
fb2_node_text (xmlNode *n)
{
  GString *out = g_string_new (NULL);
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type == XML_TEXT_NODE && c->content)
      g_string_append (out, (const char *)c->content);
    else if (c->type == XML_ELEMENT_NODE) {
      g_autofree char *inner = fb2_node_text (c);
      if (inner) g_string_append (out, inner);
    }
  }
  /* Trim. */
  while (out->len > 0 && g_ascii_isspace (out->str[out->len - 1]))
    g_string_truncate (out, out->len - 1);
  const char *s = out->str;
  while (*s && g_ascii_isspace (*s)) s++;
  if (s != out->str) {
    char *trimmed = g_strdup (s);
    g_string_free (out, TRUE);
    return trimmed;
  }
  return g_string_free (out, FALSE);
}

static void
fb2_walk_metadata (Fb2WalkCtx *cc, xmlNode *root)
{
  /* Find <description><title-info>...</title-info></description>. */
  xmlNode *title_info = NULL;
  for (xmlNode *n = root; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    if (g_ascii_strcasecmp ((const char *)n->name, "description") == 0) {
      for (xmlNode *c = n->children; c; c = c->next) {
        if (c->type == XML_ELEMENT_NODE &&
            g_ascii_strcasecmp ((const char *)c->name, "title-info") == 0) {
          title_info = c;
          break;
        }
      }
      break;
    }
  }
  if (!title_info) return;

  for (xmlNode *c = title_info->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    g_autofree char *cname = g_ascii_strdown ((const char *)c->name, -1);

    if (g_str_equal (cname, "book-title")) {
      g_autofree char *t = fb2_node_text (c);
      fb2_metadata_set (cc->doc, "title", t);
    } else if (g_str_equal (cname, "lang") || g_str_equal (cname, "src-lang")) {
      g_autofree char *t = fb2_node_text (c);
      fb2_metadata_set (cc->doc, "lang", t);
    } else if (g_str_equal (cname, "annotation")) {
      g_autofree char *t = fb2_node_text (c);
      fb2_metadata_set (cc->doc, "annotation", t);
    } else if (g_str_equal (cname, "coverpage")) {
      /* `<coverpage><image l:href="#cover_id"/></coverpage>` —
       * the FB2 spec puts a cover-image reference here. We push a
       * leading IMAGE block flagged FW_BLOCK_FLAG_COVER so the
       * viewer gives it a full page. */
      for (xmlNode *p = c->children; p; p = p->next) {
        if (p->type == XML_ELEMENT_NODE &&
            g_ascii_strcasecmp ((const char *)p->name, "image") == 0) {
          const char *href = fb2_get_href (p);
          if (href) {
            const char *id = href[0] == '#' ? href + 1 : href;
            FwBlock *cov = fw_block_new (FW_BLOCK_IMAGE, 0, NULL, id,
                                          NULL, FW_BLOCK_FLAG_COVER);
            /* Insert at head — but at metadata-walk time `blocks`
             * is empty, so append works the same. */
            g_list_store_append (cc->doc->blocks, cov);
            g_object_unref (cov);
            break;
          }
        }
      }
    } else if (g_str_equal (cname, "author")) {
      g_autofree char *fn = NULL, *mn = NULL, *ln = NULL;
      for (xmlNode *p = c->children; p; p = p->next) {
        if (p->type != XML_ELEMENT_NODE) continue;
        const char *pn = (const char *)p->name;
        if (!fn && g_ascii_strcasecmp (pn, "first-name") == 0)
          fn = fb2_node_text (p);
        else if (!mn && g_ascii_strcasecmp (pn, "middle-name") == 0)
          mn = fb2_node_text (p);
        else if (!ln && g_ascii_strcasecmp (pn, "last-name") == 0)
          ln = fb2_node_text (p);
      }
      g_autoptr (GString) au = g_string_new (NULL);
      if (fn && *fn) g_string_append (au, fn);
      if (mn && *mn) {
        if (au->len) g_string_append_c (au, ' ');
        g_string_append (au, mn);
      }
      if (ln && *ln) {
        if (au->len) g_string_append_c (au, ' ');
        g_string_append (au, ln);
      }
      if (au->len > 0)
        fb2_metadata_set (cc->doc, "author", au->str);
    }
  }
}

/* ── Binary walker ─────────────────────────────────────────────── */

static void
fb2_walk_binaries (Fb2WalkCtx *cc, xmlNode *root)
{
  /* `<binary id="..." content-type="image/...">BASE64</binary>` —
   * decode base64 → GdkTexture, store in self->images. */
  for (xmlNode *n = root; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    g_autofree char *lname = g_ascii_strdown ((const char *)n->name, -1);
    if (!g_str_equal (lname, "binary")) {
      if (n->children) fb2_walk_binaries (cc, n->children);
      continue;
    }

    const char *id = NULL;
    for (xmlAttr *a = n->properties; a; a = a->next) {
      if (g_ascii_strcasecmp ((const char *)a->name, "id") == 0 && a->children) {
        id = (const char *)a->children->content;
        break;
      }
    }
    if (!id) continue;

    g_autofree char *b64 = fb2_node_text (n);
    if (!b64 || !*b64) continue;

    gsize out_len = 0;
    g_autofree guchar *bytes = g_base64_decode (b64, &out_len);
    if (!bytes || out_len == 0) continue;

    g_autoptr (GBytes) gb = g_bytes_new (bytes, out_len);
    g_autoptr (GError) e = NULL;
    GdkTexture *tex = gdk_texture_new_from_bytes (gb, &e);
    if (tex)
      g_hash_table_insert (cc->doc->images, g_strdup (id), tex);
    else
      g_warning ("fb2: dropping image '%s': %s",
                 id, e ? e->message : "(unknown)");
  }
}

/* ── Open path ─────────────────────────────────────────────────── */

static gboolean
fb2_has_suffix_ci (const char *s, const char *suffix)
{
  gsize ls = strlen (s), lf = strlen (suffix);
  return ls >= lf && g_ascii_strcasecmp (s + ls - lf, suffix) == 0;
}

/* Read the FB2 XML bytes. A plain .fb2 is read directly; a .fb2.zip is a
 * ZIP wrapping (normally) a single .fb2 entry — extract the first .fb2
 * entry, or failing that the first regular file. Caller frees *out. */
static gboolean
fb2_read_source (const char *path, char **out, gsize *out_len, GError **error)
{
  if (!fb2_has_suffix_ci (path, ".zip"))
    return g_file_get_contents (path, out, out_len, error);

  struct archive *a = archive_read_new ();
  archive_read_support_format_zip (a);
  if (archive_read_open_filename (a, path, 16384) != ARCHIVE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "fb2: cannot open zip: %s", archive_error_string (a));
    archive_read_free (a);
    return FALSE;
  }

  struct archive_entry *entry;
  char *best = NULL;
  gsize best_len = 0;
  gboolean best_is_fb2 = FALSE;
  while (archive_read_next_header (a, &entry) == ARCHIVE_OK) {
    if (archive_entry_filetype (entry) != AE_IFREG) { archive_read_data_skip (a); continue; }
    const char *name = archive_entry_pathname (entry);
    la_int64_t size = archive_entry_size (entry);
    if (size <= 0) { archive_read_data_skip (a); continue; }
    gboolean is_fb2 = name && fb2_has_suffix_ci (name, ".fb2");
    /* Keep the first .fb2; otherwise the first regular file as fallback. */
    if (best_is_fb2 || (best && !is_fb2)) { archive_read_data_skip (a); continue; }
    char *buf = g_malloc (size);
    if (archive_read_data (a, buf, size) != size) { g_free (buf); continue; }
    g_free (best);
    best = buf;
    best_len = size;
    best_is_fb2 = is_fb2;
    if (is_fb2) break;
  }
  archive_read_free (a);

  if (!best) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "fb2: no usable entry in zip");
    return FALSE;
  }
  *out = best;
  *out_len = best_len;
  return TRUE;
}

static gboolean
fb2_open (FwReflowDocument *doc, const char *path, GError **error)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);

  g_autofree char *raw = NULL;
  gsize raw_len = 0;
  if (!fb2_read_source (path, &raw, &raw_len, error))
    return FALSE;

  /* libxml2 handles encoding declarations natively — no need to
   * pre-convert from cp1251 etc. like the GMarkupParser version did. */
  xmlDocPtr xdoc = xmlReadMemory (
    raw, (int) raw_len,
    NULL, NULL,
    XML_PARSE_RECOVER | XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
    XML_PARSE_NONET   | XML_PARSE_NOBLANKS);
  if (!xdoc) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "fb2: xmlReadMemory failed");
    return FALSE;
  }

  xmlNode *root = xmlDocGetRootElement (xdoc);
  if (!root) {
    xmlFreeDoc (xdoc);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "fb2: empty document");
    return FALSE;
  }

  Fb2WalkCtx cc = {
    .doc           = self,
    .accum         = g_string_new (NULL),
    .open_inlines  = g_ptr_array_new (),
  };

  /* Pre-pass 1: metadata (title, author, language, annotation). */
  fb2_walk_metadata (&cc, root->children);

  /* Pre-pass 2: <binary> elements → image hash. Doing this before
   * the body walk means every <image l:href="#X"/> can resolve at
   * render time, even when the binary appears after the body. */
  fb2_walk_binaries (&cc, root->children);

  /* Main pass: walk <body> producing FwBlocks. */
  for (xmlNode *n = root->children; n; n = n->next) {
    if (n->type == XML_ELEMENT_NODE &&
        g_ascii_strcasecmp ((const char *)n->name, "body") == 0) {
      fb2_walk_node (&cc, n->children);
    }
  }
  fb2_flush (&cc);

  g_string_free (cc.accum, TRUE);
  g_clear_pointer (&cc.pending_anchor, g_free);
  g_clear_pointer (&cc.toc_anchor, g_free);
  if (cc.toc_title)     g_string_free (cc.toc_title, TRUE);
  if (cc.author_first)  g_string_free (cc.author_first, TRUE);
  if (cc.author_middle) g_string_free (cc.author_middle, TRUE);
  if (cc.author_last)   g_string_free (cc.author_last, TRUE);
  g_ptr_array_free (cc.open_inlines, TRUE);
  xmlFreeDoc (xdoc);

  /* Round out metadata. */
  if (!g_hash_table_contains (self->metadata, "title")) {
    g_autofree char *basename = g_path_get_basename (path);
    g_hash_table_insert (self->metadata, g_strdup ("title"), g_strdup (basename));
  }
  g_hash_table_insert (self->metadata, g_strdup ("format"),
                       g_strdup ("FictionBook 2"));

  self->path = g_strdup (path);
  return TRUE;
}

/* ── Interface accessors ──────────────────────────────────────── */

static void
fb2_close (FwReflowDocument *doc)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  if (self->blocks) g_list_store_remove_all (self->blocks);
  if (self->toc)    g_list_store_remove_all (self->toc);
}

static GListModel *
fb2_get_block_model (FwReflowDocument *doc)
{
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_FB2 (doc)->blocks);
}

static GdkTexture *
fb2_get_image (FwReflowDocument *doc, const char *image_id)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  if (!image_id) return NULL;
  if (image_id[0] == '#') image_id++;
  return g_hash_table_lookup (self->images, image_id);
}

static GListModel *
fb2_get_toc (FwReflowDocument *doc)
{
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_FB2 (doc)->toc);
}

static guint
fb2_find_block_by_anchor (FwReflowDocument *doc, const char *anchor_id)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  if (!anchor_id) return 0;
  return GPOINTER_TO_UINT (g_hash_table_lookup (self->anchors, anchor_id));
}

static GHashTable *
fb2_get_metadata (FwReflowDocument *doc)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  return self->metadata ? g_hash_table_ref (self->metadata) : NULL;
}

static void
fw_reflow_document_fb2_iface_init (FwReflowDocumentInterface *iface)
{
  iface->open                  = fb2_open;
  iface->close                 = fb2_close;
  iface->get_block_model       = fb2_get_block_model;
  iface->get_image             = fb2_get_image;
  iface->get_toc               = fb2_get_toc;
  iface->find_block_by_anchor  = fb2_find_block_by_anchor;
  iface->get_metadata          = fb2_get_metadata;
}

/* ── GObject boilerplate ──────────────────────────────────────── */

static void
fw_reflow_document_fb2_finalize (GObject *object)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (object);
  g_clear_object (&self->blocks);
  g_clear_object (&self->toc);
  g_clear_pointer (&self->metadata, g_hash_table_unref);
  g_clear_pointer (&self->images,   g_hash_table_unref);
  g_clear_pointer (&self->anchors,  g_hash_table_unref);
  g_clear_pointer (&self->path,     g_free);
  G_OBJECT_CLASS (fw_reflow_document_fb2_parent_class)->finalize (object);
}

static void
fw_reflow_document_fb2_class_init (FwReflowDocumentFb2Class *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fw_reflow_document_fb2_finalize;
}

static void
fw_reflow_document_fb2_init (FwReflowDocumentFb2 *self)
{
  self->blocks   = g_list_store_new (FW_TYPE_BLOCK);
  self->toc      = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
  self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, g_free);
  self->images   = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, g_object_unref);
  self->anchors  = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, NULL);
}

FwReflowDocumentFb2 *
fw_reflow_document_fb2_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_DOCUMENT_FB2, NULL);
}
