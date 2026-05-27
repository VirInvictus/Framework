/* fw-reflow-document-mobi.c — MOBI / KF7 / KF8 reflow backend
 *
 * Uses fw-mobi-parser to extract a concatenated UTF-8 HTML body
 * (KF7: raw decompressed text; KF8: SKEL+FRAG-spliced sections),
 * then walks it with libxml2's htmlReadMemory + tree walker to
 * produce FwBlocks. libxml2's HTML mode is tolerant of malformed
 * markup the same way Foliate's DOMParser is — orphan close tags,
 * unclosed elements, unquoted attributes, embedded XML decls all
 * survive parsing. This matches `.foliate-js/mobi.js`'s behavior.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document-mobi.h"
#include "fw-mobi-parser.h"

#include <string.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

struct _FwReflowDocumentMobi {
  GObject       parent_instance;
  GListStore   *blocks;
  GListStore   *toc;
  GHashTable   *metadata;
  GHashTable   *anchors;
  GHashTable   *images;     /* gchar* (recindex string) → GdkTexture */
  char         *path;
};

static void fw_reflow_document_mobi_iface_init (FwReflowDocumentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwReflowDocumentMobi,
                               fw_reflow_document_mobi,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (FW_TYPE_REFLOW_DOCUMENT,
                                                      fw_reflow_document_mobi_iface_init))

/* (Tag-balancer removed — libxml2's HTML mode handles malformed
 * markup natively. Kept here as a stub so the pre-libxml2 string
 * is gone. The MOBI body goes straight into htmlReadMemory now.) */

/* ── libxml2 HTML walker ─────────────────────────────────────────
 *
 * Foliate parses MOBI HTML through DOMParser — tolerant of every
 * malformation real MOBIs throw at it (orphan closes, unclosed
 * tags, unquoted attributes, embedded XML decls). libxml2's
 * `htmlReadMemory` with HTML_PARSE_RECOVER + NOERROR + NOWARNING
 * gives us the same tolerance, plus a real DOM tree we walk
 * preorder, dispatching by tag name.
 */

typedef struct {
  GListStore  *blocks;
  GHashTable  *anchors;
  gboolean     in_body;

  /* Accumulator for the current block-level element. */
  gboolean     accum_active;
  FwBlockKind  accum_kind;
  int          accum_level;
  guint        accum_flags;     /* extra FW_BLOCK_FLAG_* bits */
  GString     *accum;
  char        *pending_anchor;

  /* Stack of inline Pango tag names that are currently open inside
   * the accumulator (e.g. {"i", "b"}). Tracked so that when an
   * inline span crosses a block-level boundary (an `<a>` wrapping
   * multiple `<p>`s, common in real HTML), we can auto-close on
   * flush and re-open on the next block. */
  GPtrArray   *open_inlines;

  /* Ordered-list nesting. Each element is the next number to emit
   * (1+) for an `<ol>`; 0 = `<ul>` (unordered). Top-of-stack is the
   * innermost list. */
  GArray      *ol_stack;
} WalkCtx;

static int
heading_level (const char *name)
{
  if (g_str_equal (name, "h1")) return 1;
  if (g_str_equal (name, "h2")) return 2;
  if (g_str_equal (name, "h3")) return 3;
  if (g_str_equal (name, "h4")) return 4;
  if (g_str_equal (name, "h5")) return 5;
  if (g_str_equal (name, "h6")) return 6;
  return 0;
}

static const char *
pango_tag_for_inline (const char *lname)
{
  if (g_str_equal (lname, "em") || g_str_equal (lname, "i"))      return "i";
  if (g_str_equal (lname, "strong") || g_str_equal (lname, "b"))  return "b";
  if (g_str_equal (lname, "code") || g_str_equal (lname, "tt") ||
      g_str_equal (lname, "kbd")  || g_str_equal (lname, "samp")) return "tt";
  if (g_str_equal (lname, "sub"))                                  return "sub";
  if (g_str_equal (lname, "sup"))                                  return "sup";
  if (g_str_equal (lname, "s") || g_str_equal (lname, "strike") ||
      g_str_equal (lname, "del"))                                  return "s";
  if (g_str_equal (lname, "u") || g_str_equal (lname, "ins") ||
      g_str_equal (lname, "a"))                                    return "u";
  return NULL;
}

static void
flush_accum (WalkCtx *cc)
{
  if (!cc->accum_active) return;
  cc->accum_active = FALSE;

  /* Close any inlines still open inside this block before flushing.
   * They get re-opened on the next block via re_emit_open_inlines. */
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
                               cc->pending_anchor, cc->accum_flags);
    g_list_store_append (cc->blocks, b);
    if (cc->pending_anchor) {
      guint pos = g_list_model_get_n_items (G_LIST_MODEL (cc->blocks)) - 1;
      g_hash_table_insert (cc->anchors, g_strdup (cc->pending_anchor),
                           GUINT_TO_POINTER (pos + 1));
    }
    g_object_unref (b);
  }
  g_string_truncate (cc->accum, 0);
  g_clear_pointer (&cc->pending_anchor, g_free);
  cc->accum_level = 0;
  cc->accum_flags = 0;
}

static void
re_emit_open_inlines (WalkCtx *cc)
{
  /* When a new block starts inside still-active inline spans
   * (e.g. <a>...<p>...</p>...</a>), re-emit each open span at the
   * start of the new accumulator so the rendered output keeps the
   * styling. */
  if (!cc->open_inlines) return;
  for (guint i = 0; i < cc->open_inlines->len; i++)
    g_string_append_printf (cc->accum, "<%s>",
                            (const char *) cc->open_inlines->pdata[i]);
}

static void
start_accum (WalkCtx *cc, FwBlockKind kind, int level,
             xmlNode *element)
{
  flush_accum (cc);
  cc->accum_active = TRUE;
  cc->accum_kind   = kind;
  cc->accum_level  = level;
  /* Pull `id` attribute for anchor resolution. */
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
  /* Re-open any inline spans that were active across the block
   * boundary (e.g. an enclosing `<a>` that wraps multiple `<p>`s). */
  re_emit_open_inlines (cc);
}

static void
append_text_escaped (GString *out, const char *text)
{
  if (!text) return;
  /* Collapse whitespace runs to a single space (libxml2 normalises
   * the source HTML to UTF-8 but doesn't squash whitespace; doing
   * it here avoids leaking source-side indentation into rendered
   * paragraphs). */
  for (const char *p = text; *p; p++) {
    char c = *p;
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && out->len > 0 && out->str[out->len - 1] == ' ') continue;
    if      (c == '<') g_string_append (out, "&lt;");
    else if (c == '>') g_string_append (out, "&gt;");
    else if (c == '&') g_string_append (out, "&amp;");
    else               g_string_append_c (out, c);
  }
}

static void walk_xml_node (WalkCtx *cc, xmlNode *node);

/* Register an `id` attribute on any element (block or nested) as
 * an anchor pointing at the next-to-be-pushed block index. After
 * the synthetic `<span id="filepos_N">` markers from the
 * pre-scan land mid-paragraph, this is how `filepos:N` resolves
 * in TOC navigation. */
static void
register_inline_id (WalkCtx *cc, xmlNode *n)
{
  for (xmlAttr *a = n->properties; a; a = a->next) {
    if (g_ascii_strcasecmp ((const char *)a->name, "id") == 0 && a->children) {
      const char *val = (const char *)a->children->content;
      if (val && *val) {
        guint pos = g_list_model_get_n_items (G_LIST_MODEL (cc->blocks));
        /* +1 because anchors are stored 1-based (0 = "not found"). */
        g_hash_table_insert (cc->anchors, g_strdup (val),
                             GUINT_TO_POINTER (pos + 1));
      }
      return;
    }
  }
}

static void
handle_element (WalkCtx *cc, xmlNode *n)
{
  const char *name = (const char *)n->name;
  /* Make a lowercase copy for case-insensitive matching. */
  g_autofree char *lname = g_ascii_strdown (name, -1);

  /* Capture id attributes on every element so synthetic
   * `<span id="filepos_N">` markers land in the anchor map. */
  register_inline_id (cc, n);

  /* MOBI break / section markers — push directly. */
  if (g_str_equal (lname, "mbp:pagebreak") ||
      g_str_equal (lname, "pagebreak")) {
    flush_accum (cc);
    FwBlock *hr = fw_block_new (FW_BLOCK_HR, 0, NULL, NULL, NULL, 0);
    g_list_store_append (cc->blocks, hr);
    g_object_unref (hr);
    return;
  }
  if (g_str_equal (lname, "mbp:section") || g_str_equal (lname, "section")) {
    flush_accum (cc);
    const char *id = NULL;
    for (xmlAttr *a = n->properties; a; a = a->next) {
      if (g_ascii_strcasecmp ((const char *)a->name, "id") == 0 && a->children) {
        id = (const char *)a->children->content;
        break;
      }
    }
    FwBlock *chap = fw_block_new (FW_BLOCK_CHAPTER, 0, NULL, NULL, id, 0);
    g_list_store_append (cc->blocks, chap);
    if (id && *id) {
      guint pos = g_list_model_get_n_items (G_LIST_MODEL (cc->blocks)) - 1;
      g_hash_table_insert (cc->anchors, g_strdup (id), GUINT_TO_POINTER (pos + 1));
    }
    g_object_unref (chap);
    /* Section is a container — walk its children for content. */
    walk_xml_node (cc, n->children);
    return;
  }

  int hl = heading_level (lname);
  if (hl > 0) {
    start_accum (cc, FW_BLOCK_HEADING, hl, n);
    walk_xml_node (cc, n->children);
    flush_accum (cc);
    return;
  }
  if (g_str_equal (lname, "p")) {
    start_accum (cc, FW_BLOCK_PARAGRAPH, 0, n);
    walk_xml_node (cc, n->children);
    flush_accum (cc);
    return;
  }
  if (g_str_equal (lname, "blockquote")) {
    start_accum (cc, FW_BLOCK_BLOCKQUOTE, 0, n);
    walk_xml_node (cc, n->children);
    flush_accum (cc);
    return;
  }
  if (g_str_equal (lname, "pre")) {
    start_accum (cc, FW_BLOCK_CODE, 0, n);
    walk_xml_node (cc, n->children);
    flush_accum (cc);
    return;
  }
  if (g_str_equal (lname, "ul") || g_str_equal (lname, "ol")) {
    int slot = g_str_equal (lname, "ol") ? 1 : 0;
    g_array_append_val (cc->ol_stack, slot);
    walk_xml_node (cc, n->children);
    g_array_set_size (cc->ol_stack, cc->ol_stack->len - 1);
    return;
  }
  if (g_str_equal (lname, "li")) {
    int level = 0;
    if (cc->ol_stack && cc->ol_stack->len > 0) {
      int *top = &g_array_index (cc->ol_stack, int, cc->ol_stack->len - 1);
      if (*top > 0) { level = *top; (*top)++; }
    }
    start_accum (cc, FW_BLOCK_LIST_ITEM, level, n);
    walk_xml_node (cc, n->children);
    flush_accum (cc);
    return;
  }
  if (g_str_equal (lname, "figure")) {
    walk_xml_node (cc, n->children);
    return;
  }
  if (g_str_equal (lname, "figcaption")) {
    start_accum (cc, FW_BLOCK_PARAGRAPH, 0, n);
    cc->accum_flags |= FW_BLOCK_FLAG_CAPTION;
    walk_xml_node (cc, n->children);
    flush_accum (cc);
    return;
  }
  if (g_str_equal (lname, "hr")) {
    flush_accum (cc);
    FwBlock *hr = fw_block_new (FW_BLOCK_HR, 0, NULL, NULL, NULL, 0);
    g_list_store_append (cc->blocks, hr);
    g_object_unref (hr);
    return;
  }
  if (g_str_equal (lname, "br")) {
    if (cc->accum_active) g_string_append_c (cc->accum, '\n');
    return;
  }

  /* MOBI <img recindex="N"> — push IMAGE block. */
  if (g_str_equal (lname, "img")) {
    flush_accum (cc);
    const char *recindex = NULL;
    for (xmlAttr *a = n->properties; a; a = a->next) {
      if (g_ascii_strcasecmp ((const char *)a->name, "recindex") == 0 && a->children) {
        recindex = (const char *)a->children->content;
        break;
      }
    }
    if (recindex && *recindex) {
      FwBlock *img = fw_block_new (FW_BLOCK_IMAGE, 0, NULL,
                                    recindex, NULL, 0);
      g_list_store_append (cc->blocks, img);
      g_object_unref (img);
    }
    return;
  }

  /* Inline tags inside an active accumulator — append open / recurse
   * / close, AND track the inline on a stack so we can balance
   * across block boundaries when the source HTML uses an inline
   * to wrap multiple block-level elements. */
  if (cc->accum_active) {
    const char *pname = pango_tag_for_inline (lname);
    if (pname) {
      g_string_append_printf (cc->accum, "<%s>", pname);
      g_ptr_array_add (cc->open_inlines, (gpointer) pname);
      walk_xml_node (cc, n->children);
      /* Pop only if still on top (flush_accum may have run during
       * recursion). Append the close ONLY if we're still inside an
       * active block accumulator — otherwise the close-tag would
       * leak into whatever block starts next, producing
       * "</u>Or visit us online at" GtkLabel-markup errors. */
      if (cc->open_inlines->len > 0 &&
          cc->open_inlines->pdata[cc->open_inlines->len - 1] == pname) {
        g_ptr_array_remove_index (cc->open_inlines,
                                   cc->open_inlines->len - 1);
        if (cc->accum_active)
          g_string_append_printf (cc->accum, "</%s>", pname);
      }
    } else {
      walk_xml_node (cc, n->children);
    }
    return;
  }

  /* Container we don't specifically recognise — recurse anyway so
   * deeper block elements still get parsed. */
  walk_xml_node (cc, n->children);
}

static void
walk_xml_node (WalkCtx *cc, xmlNode *node)
{
  for (xmlNode *n = node; n; n = n->next) {
    if (n->type == XML_ELEMENT_NODE) {
      const char *name = (const char *)n->name;
      g_autofree char *lname = g_ascii_strdown (name, -1);

      /* `<body>` opens accumulator state; `<head>` is skipped. */
      if (g_str_equal (lname, "body")) {
        gboolean was = cc->in_body;
        cc->in_body = TRUE;
        walk_xml_node (cc, n->children);
        cc->in_body = was;
        continue;
      }
      if (g_str_equal (lname, "head") || g_str_equal (lname, "style") ||
          g_str_equal (lname, "script") || g_str_equal (lname, "title")) {
        continue;
      }
      if (!cc->in_body) {
        /* For top-level elements like <html>, walk children. */
        if (g_str_equal (lname, "html")) {
          walk_xml_node (cc, n->children);
          continue;
        }
        /* MOBI body bytes sometimes lack <html><body>; treat the
         * whole tree as body content if we never saw <body>. */
        cc->in_body = TRUE;
        handle_element (cc, n);
        cc->in_body = FALSE;
        continue;
      }
      handle_element (cc, n);
    } else if (n->type == XML_TEXT_NODE && cc->accum_active) {
      append_text_escaped (cc->accum, (const char *)n->content);
    }
  }
}

/* ── filepos anchors + guide TOC ─────────────────────────────────
 *
 * MOBI uses byte offsets ("filepos") to point at link targets in
 * the original decompressed body. Foliate handles this by:
 *
 *   1. Scanning the body for every `filepos="N"` value
 *   2. Inserting a synthetic `<a id="filepos${N}"></a>` at each
 *      byte offset N
 *   3. After parse, the DOM has anchors at every filepos target
 *
 * We do the same — except we use `<span>` for the marker since
 * `<a>` would become a Pango `<u>` span in our walker. The walker
 * captures every element id (via register_inline_id) so the
 * synthetic markers land in the anchors hash with key `filepos_N`.
 *
 * `<reference type="toc" filepos="N" title="X">` elements (typically
 * inside `<guide>`) become TOC entries with anchor `filepos_N`.
 */

static int
cmp_uint (gconstpointer a, gconstpointer b)
{
  guint32 av = *(const guint32 *) a, bv = *(const guint32 *) b;
  return (av > bv) - (av < bv);
}

/* Scan `body` for unique filepos numeric values. */
static GArray *
mobi_collect_filepos (const char *body, gsize len)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (guint32));
  GHashTable *seen = g_hash_table_new (g_direct_hash, g_direct_equal);

  const char *end = body + len;
  const char *p = body;
  const char *needle = "filepos=";
  gsize nlen = 8;

  while (p + nlen <= end) {
    p = g_strstr_len (p, end - p, needle);
    if (!p) break;
    p += nlen;
    char q = 0;
    if (p < end && (*p == '"' || *p == '\'')) { q = *p; p++; }
    (void) q;

    guint32 val = 0;
    const char *digits = p;
    while (p < end && *p >= '0' && *p <= '9') {
      if (val > UINT32_MAX / 10) { p = end; break; }
      val = val * 10 + (guint32)(*p - '0');
      p++;
    }
    if (p > digits) {
      gpointer key = GUINT_TO_POINTER (val);
      if (!g_hash_table_contains (seen, key)) {
        g_hash_table_add (seen, key);
        g_array_append_val (out, val);
      }
    }
  }
  g_hash_table_destroy (seen);
  g_array_sort (out, cmp_uint);
  return out;
}

/* Build a new body by inserting `<span id="<prefix><N>"></span>` markers
 * at each byte offset in `positions` (which MUST be ascending, so
 * earlier insertions don't shift later positions). Used for both KF7
 * filepos anchors (prefix "filepos_") and KF8 NCX anchors (prefix
 * "ncx_"); in both cases the marker id encodes the offset so the TOC
 * entry can reference it. */
static char *
mobi_inject_markers (const char *body, gsize body_len,
                     GArray *positions, const char *id_prefix, gsize *out_len)
{
  if (!positions || positions->len == 0) {
    *out_len = body_len;
    return g_memdup2 (body, body_len + 1);
  }

  GString *out = g_string_sized_new (body_len + positions->len * 32);
  gsize cursor = 0;
  for (guint i = 0; i < positions->len; i++) {
    guint32 pos = g_array_index (positions, guint32, i);
    if (pos > body_len) pos = body_len;
    if (pos > cursor)
      g_string_append_len (out, body + cursor, pos - cursor);
    g_string_append_printf (out, "<span id=\"%s%u\"></span>", id_prefix, pos);
    cursor = pos;
  }
  if (cursor < body_len)
    g_string_append_len (out, body + cursor, body_len - cursor);
  *out_len = out->len;
  return g_string_free (out, FALSE);
}

static gint
cmp_guint32 (gconstpointer a, gconstpointer b)
{
  guint32 x = *(const guint32 *) a, y = *(const guint32 *) b;
  return (x > y) - (x < y);
}

/* Concatenate descendant text content of a node, normalised to
 * single-space whitespace and trimmed. Used for TOC labels. */
static char *
mobi_node_text (xmlNode *node)
{
  GString *out = g_string_new (NULL);
  for (xmlNode *c = node; c; c = c->next) {
    if (c->type == XML_TEXT_NODE && c->content)
      g_string_append (out, (const char *)c->content);
    else if (c->type == XML_ELEMENT_NODE) {
      g_autofree char *inner = mobi_node_text (c->children);
      if (inner) g_string_append (out, inner);
    }
  }
  /* Squash whitespace runs to single space. */
  GString *sq = g_string_new (NULL);
  gboolean prev_ws = TRUE;
  for (gsize i = 0; i < out->len; i++) {
    char ch = out->str[i];
    if (g_ascii_isspace (ch)) {
      if (!prev_ws) g_string_append_c (sq, ' ');
      prev_ws = TRUE;
    } else {
      g_string_append_c (sq, ch);
      prev_ws = FALSE;
    }
  }
  while (sq->len > 0 && g_ascii_isspace (sq->str[sq->len - 1]))
    g_string_truncate (sq, sq->len - 1);
  g_string_free (out, TRUE);
  return g_string_free (sq, FALSE);
}

/* Walk for `<reference type="toc" filepos="N" title=X>` — typical
 * `<guide>` chapter list. Returns count emitted. */
static guint
mobi_walk_references (FwReflowDocumentMobi *self, xmlNode *node)
{
  guint count = 0;
  for (xmlNode *n = node; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) {
      if (n->children) count += mobi_walk_references (self, n->children);
      continue;
    }
    if (g_ascii_strcasecmp ((const char *)n->name, "reference") == 0) {
      const char *title = NULL, *filepos = NULL, *type = NULL;
      for (xmlAttr *a = n->properties; a; a = a->next) {
        if (!a->children) continue;
        const char *aname = (const char *)a->name;
        const char *aval  = (const char *)a->children->content;
        if (g_ascii_strcasecmp (aname, "title") == 0)        title   = aval;
        else if (g_ascii_strcasecmp (aname, "filepos") == 0) filepos = aval;
        else if (g_ascii_strcasecmp (aname, "type") == 0)    type    = aval;
      }
      /* Skip cover/copyright references — they're navigational
       * markers, not chapter entries. Only emit when we have a
       * non-cover type (or no type at all). */
      if (type && (g_ascii_strcasecmp (type, "cover") == 0 ||
                   g_ascii_strcasecmp (type, "copyright-page") == 0)) {
        if (n->children) count += mobi_walk_references (self, n->children);
        continue;
      }
      if (title && filepos && *title && *filepos) {
        g_autofree char *anchor = g_strdup_printf ("filepos_%s", filepos);
        FwReflowTocItem *item = fw_reflow_toc_item_new (title, anchor);
        g_list_store_append (self->toc, item);
        g_object_unref (item);
        count++;
      }
    }
    if (n->children) count += mobi_walk_references (self, n->children);
  }
  return count;
}

/* Fallback: walk `<a filepos="N">link text</a>` elements anywhere
 * in the doc. Used when the `<guide>` is empty or missing. Some
 * MOBIs encode their TOC purely as in-body links. */
static guint
mobi_walk_a_filepos (FwReflowDocumentMobi *self, xmlNode *node)
{
  guint count = 0;
  for (xmlNode *n = node; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) {
      if (n->children) count += mobi_walk_a_filepos (self, n->children);
      continue;
    }
    if (g_ascii_strcasecmp ((const char *)n->name, "a") == 0) {
      const char *filepos = NULL;
      for (xmlAttr *a = n->properties; a; a = a->next) {
        if (a->children &&
            g_ascii_strcasecmp ((const char *)a->name, "filepos") == 0) {
          filepos = (const char *)a->children->content;
          break;
        }
      }
      if (filepos && *filepos) {
        g_autofree char *label = mobi_node_text (n->children);
        if (label && *label) {
          g_autofree char *anchor = g_strdup_printf ("filepos_%s", filepos);
          FwReflowTocItem *item = fw_reflow_toc_item_new (label, anchor);
          g_list_store_append (self->toc, item);
          g_object_unref (item);
          count++;
        }
      }
    }
    if (n->children) count += mobi_walk_a_filepos (self, n->children);
  }
  return count;
}

static void
mobi_walk_guide (FwReflowDocumentMobi *self, xmlNode *root, gboolean has_filepos)
{
  guint refs = mobi_walk_references (self, root);
  /* If `<guide>` produced just a stub (one or zero entries — typical
   * when only a "cover" reference is present) and the doc carries
   * in-body filepos markers, fall back to walking `<a filepos>` so
   * the chapter list still surfaces. */
  if (refs <= 1 && has_filepos) {
    g_list_store_remove_all (self->toc);   /* drop the stub */
    mobi_walk_a_filepos (self, root);
  }
}

/* ── Open path ────────────────────────────────────────────────── */

static gboolean
mobi_open (FwReflowDocument *doc, const char *path, GError **error)
{
  FwReflowDocumentMobi *self = FW_REFLOW_DOCUMENT_MOBI (doc);

  FwMobiParsed *m = fw_mobi_parse (path, error);
  if (!m) return FALSE;

  /* Metadata. */
  if (m->title) {
    g_hash_table_insert (self->metadata, g_strdup ("title"), g_strdup (m->title));
  } else {
    g_autofree char *bn = g_path_get_basename (path);
    g_hash_table_insert (self->metadata, g_strdup ("title"), g_strdup (bn));
  }
  if (m->author)    g_hash_table_insert (self->metadata, g_strdup ("author"),    g_strdup (m->author));
  if (m->publisher) g_hash_table_insert (self->metadata, g_strdup ("publisher"), g_strdup (m->publisher));
  if (m->language)  g_hash_table_insert (self->metadata, g_strdup ("lang"),      g_strdup (m->language));
  g_hash_table_insert (self->metadata, g_strdup ("format"),
                       g_strdup (m->is_kf8 ? "Mobipocket (KF8 / AZW3)"
                                           : "Mobipocket (KF7)"));

  /* Steal the image hash from the parsed struct — it carries
   * already-decoded GdkTextures keyed by recindex string. */
  if (m->images) {
    g_clear_pointer (&self->images, g_hash_table_unref);
    self->images = m->images;
    m->images = NULL;
  }

  /* If a cover image was identified via EXTH-201, push it as the
   * first block — flagged FW_BLOCK_FLAG_COVER so FwReflowView
   * gives it a full-viewport page. */
  if (m->cover_recindex > 0) {
    g_autofree char *id = g_strdup_printf ("%u", m->cover_recindex);
    FwBlock *cover = fw_block_new (FW_BLOCK_IMAGE, 0, NULL, id, NULL,
                                    FW_BLOCK_FLAG_COVER);
    g_list_store_append (self->blocks, cover);
    g_object_unref (cover);
  }

  if (m->body_len == 0) {
    /* Empty body — open succeeds with metadata only; user sees an
     * empty document. Better than failing the open path. */
    self->path = g_strdup (path);
    fw_mobi_parsed_free (m);
    return TRUE;
  }

  /* TOC anchor source. KF8/AZW3 carries an NCX index (extracted by the
   * parser into m->toc with byte offsets into the spliced body); KF7
   * uses in-body filepos byte offsets. Either way we inject synthetic
   * id-bearing markers at those offsets so libxml2's tree carries them
   * and find_block_by_anchor can resolve TOC targets. */
  gboolean kf8_toc = (m->is_kf8 && m->toc && m->toc->len > 0);
  g_autoptr (GArray) positions = NULL;
  const char *marker_prefix;
  if (kf8_toc) {
    positions = g_array_new (FALSE, FALSE, sizeof (guint32));
    for (guint i = 0; i < m->toc->len; i++) {
      guint32 o = g_array_index (m->toc, FwMobiTocEntry, i).body_offset;
      g_array_append_val (positions, o);
    }
    g_array_sort (positions, cmp_guint32);   /* injector needs ascending */
    marker_prefix = "ncx_";
  } else {
    positions = mobi_collect_filepos (m->body, m->body_len);  /* sorted, unique */
    marker_prefix = "filepos_";
  }

  gsize body_len2 = 0;
  g_autofree char *body2 = mobi_inject_markers (
    m->body, m->body_len, positions, marker_prefix, &body_len2);

  /* libxml2 HTML mode — tolerant of malformed markup. The flags
   * RECOVER (continue on error) + NOERROR / NOWARNING (don't print
   * to stderr) match Foliate's DOMParser semantics. */
  htmlDocPtr xdoc = htmlReadMemory (
    body2, (int) body_len2,
    NULL, "UTF-8",
    HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING |
    HTML_PARSE_NONET    | HTML_PARSE_NOBLANKS);
  if (!xdoc) {
    g_warning ("mobi: htmlReadMemory returned NULL — keeping %u blocks",
               g_list_model_get_n_items (G_LIST_MODEL (self->blocks)));
    fw_mobi_parsed_free (m);
    self->path = g_strdup (path);
    return TRUE;
  }

  WalkCtx cc = {
    .blocks        = self->blocks,
    .anchors       = self->anchors,
    .accum         = g_string_new (NULL),
    .open_inlines  = g_ptr_array_new (),  /* refs are static strings */
    .ol_stack      = g_array_new (FALSE, FALSE, sizeof (int)),
  };
  walk_xml_node (&cc, xmlDocGetRootElement (xdoc));
  flush_accum (&cc);
  g_string_free (cc.accum, TRUE);
  g_clear_pointer (&cc.pending_anchor, g_free);
  g_ptr_array_free (cc.open_inlines, TRUE);
  g_array_free (cc.ol_stack, TRUE);

  /* TOC. KF8: build directly from the parser's NCX entries, in
   * document order, each pointing at its injected ncx_<offset> anchor.
   * KF7: walk `<reference>`/`<guide>` filepos entries. Both run after
   * the body walk so the anchors are registered first. */
  if (kf8_toc) {
    for (guint i = 0; i < m->toc->len; i++) {
      FwMobiTocEntry *te = &g_array_index (m->toc, FwMobiTocEntry, i);
      g_autofree char *anchor = g_strdup_printf ("ncx_%u", te->body_offset);
      FwReflowTocItem *item = fw_reflow_toc_item_new (te->label, anchor);
      g_list_store_append (self->toc, item);
      g_object_unref (item);
    }
  } else {
    mobi_walk_guide (self, xmlDocGetRootElement (xdoc),
                     positions && positions->len > 0);
  }

  xmlFreeDoc (xdoc);

  fw_mobi_parsed_free (m);
  self->path = g_strdup (path);
  return TRUE;
}

static void mobi_close (FwReflowDocument *doc) {
  FwReflowDocumentMobi *self = FW_REFLOW_DOCUMENT_MOBI (doc);
  if (self->blocks) g_list_store_remove_all (self->blocks);
}
static GListModel *mobi_get_block_model (FwReflowDocument *doc) {
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_MOBI (doc)->blocks);
}
static GdkTexture *mobi_get_image (FwReflowDocument *doc, const char *id) {
  if (!id) return NULL;
  FwReflowDocumentMobi *self = FW_REFLOW_DOCUMENT_MOBI (doc);
  if (!self->images) return NULL;
  return g_hash_table_lookup (self->images, id);
}
static GListModel *mobi_get_toc (FwReflowDocument *doc) {
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_MOBI (doc)->toc);
}
static guint mobi_find_block_by_anchor (FwReflowDocument *doc, const char *a) {
  if (!a) return 0;
  return GPOINTER_TO_UINT (
    g_hash_table_lookup (FW_REFLOW_DOCUMENT_MOBI (doc)->anchors, a));
}
static GHashTable *mobi_get_metadata (FwReflowDocument *doc) {
  FwReflowDocumentMobi *self = FW_REFLOW_DOCUMENT_MOBI (doc);
  return self->metadata ? g_hash_table_ref (self->metadata) : NULL;
}

static void
fw_reflow_document_mobi_iface_init (FwReflowDocumentInterface *iface)
{
  iface->open                  = mobi_open;
  iface->close                 = mobi_close;
  iface->get_block_model       = mobi_get_block_model;
  iface->get_image             = mobi_get_image;
  iface->get_toc               = mobi_get_toc;
  iface->find_block_by_anchor  = mobi_find_block_by_anchor;
  iface->get_metadata          = mobi_get_metadata;
}

static void
fw_reflow_document_mobi_finalize (GObject *object)
{
  FwReflowDocumentMobi *self = FW_REFLOW_DOCUMENT_MOBI (object);
  g_clear_object (&self->blocks);
  g_clear_object (&self->toc);
  g_clear_pointer (&self->metadata, g_hash_table_unref);
  g_clear_pointer (&self->anchors,  g_hash_table_unref);
  g_clear_pointer (&self->images,   g_hash_table_unref);
  g_clear_pointer (&self->path,     g_free);
  G_OBJECT_CLASS (fw_reflow_document_mobi_parent_class)->finalize (object);
}

static void
fw_reflow_document_mobi_class_init (FwReflowDocumentMobiClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fw_reflow_document_mobi_finalize;
}

static void
fw_reflow_document_mobi_init (FwReflowDocumentMobi *self)
{
  self->blocks   = g_list_store_new (FW_TYPE_BLOCK);
  self->toc      = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
  self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->anchors  = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->images   = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, g_object_unref);
}

FwReflowDocumentMobi *
fw_reflow_document_mobi_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_DOCUMENT_MOBI, NULL);
}
