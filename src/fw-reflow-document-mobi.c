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

static gboolean
is_void_html (const char *name)
{
  return g_str_equal (name, "br")    || g_str_equal (name, "hr") ||
         g_str_equal (name, "img")   || g_str_equal (name, "meta") ||
         g_str_equal (name, "link")  || g_str_equal (name, "input") ||
         g_str_equal (name, "area")  || g_str_equal (name, "base") ||
         g_str_equal (name, "col")   || g_str_equal (name, "embed") ||
         g_str_equal (name, "param") || g_str_equal (name, "source") ||
         g_str_equal (name, "track") || g_str_equal (name, "wbr") ||
         g_str_equal (name, "mbp:pagebreak") ||
         g_str_equal (name, "pagebreak");
}

#if 0  /* — old GMarkupParser balancer / walker kept for reference — */
static char *
balance_html (const char *html, gsize len, gsize *out_len)
{
  GString   *out = g_string_sized_new (len + 256);
  GPtrArray *stk = g_ptr_array_new_with_free_func (g_free);

  gsize i = 0;
  while (i < len) {
    if (html[i] != '<') {
      g_string_append_c (out, html[i++]);
      continue;
    }

    /* Comments / CDATA / doctype / PI — drop entirely. KF8 spliced
     * sections often have `<?xml version="1.0"?>` at section
     * boundaries (one per fragment), and GMarkupParser only allows
     * a single XML declaration at the start. Easiest path: strip
     * all of these. They carry no rendered content. */
    if (i + 1 < len && (html[i + 1] == '!' || html[i + 1] == '?')) {
      gsize end = i;
      while (end < len && html[end] != '>') end++;
      if (end < len) end++;
      i = end;
      continue;
    }

    /* Find the matching '>' (track quotes so embedded > inside
     * attributes doesn't fool us). */
    gsize end = i + 1;
    char quote = 0;
    while (end < len) {
      char c = html[end];
      if (quote) { if (c == quote) quote = 0; }
      else if (c == '"' || c == '\'') quote = c;
      else if (c == '>') break;
      end++;
    }
    if (end >= len) {
      g_string_append_len (out, html + i, len - i);
      break;
    }

    gboolean is_close = (i + 1 < len && html[i + 1] == '/');
    gsize name_start = i + (is_close ? 2 : 1);
    gsize name_end = name_start;
    while (name_end < end &&
           !g_ascii_isspace (html[name_end]) &&
           html[name_end] != '/' &&
           html[name_end] != '>')
      name_end++;
    g_autofree char *raw_name = g_strndup (html + name_start,
                                            name_end - name_start);
    g_autofree char *name = g_ascii_strdown (raw_name, -1);

    gboolean self_close = (end >= 1 && html[end - 1] == '/');

    if (is_close) {
      gint match = -1;
      for (gint s = (gint) stk->len - 1; s >= 0; s--)
        if (g_str_equal (stk->pdata[s], name)) { match = s; break; }
      if (match >= 0) {
        for (guint s = stk->len - 1; (gint) s >= match; s--)
          g_string_append_printf (out, "</%s>", (const char *) stk->pdata[s]);
        g_ptr_array_set_size (stk, match);
      }
      i = end + 1;
      continue;
    }

    /* Open tag — emit byte by byte, quoting unquoted attribute
     * values on the fly, and self-closing void elements so a
     * strict XML parser doesn't complain. State machine:
     *   normal   → looking for '=' signaling start of attr value
     *   after_eq → just saw '='; if next is " or ', leave alone;
     *              otherwise quote the value to next ws / / / >.
     */
    gboolean void_el = is_void_html (name);

    g_string_append_c (out, '<');
    if (is_close) g_string_append_c (out, '/');
    gsize p = name_start;
    int state = 0;
    char active_q = 0;
    while (p < end) {            /* stop BEFORE the closing '>' */
      char c = html[p];
      if (active_q) {
        g_string_append_c (out, c);
        if (c == active_q) active_q = 0;
        p++;
        continue;
      }
      if (state == 0) {
        if (c == '=') { state = 1; g_string_append_c (out, c); p++; continue; }
        g_string_append_c (out, c);
        p++;
        continue;
      }
      /* state == 1, just emitted '=' */
      if (c == '"' || c == '\'') {
        active_q = c;
        g_string_append_c (out, c);
        p++;
        state = 0;
        continue;
      }
      /* Unquoted value — wrap until next ws/>// */
      g_string_append_c (out, '"');
      while (p < end &&
             !g_ascii_isspace (html[p]) &&
             html[p] != '>' &&
             html[p] != '/') {
        char vc = html[p];
        if      (vc == '"') g_string_append (out, "&quot;");
        else if (vc == '<') g_string_append (out, "&lt;");
        else if (vc == '>') g_string_append (out, "&gt;");
        else if (vc == '&') g_string_append (out, "&amp;");
        else                g_string_append_c (out, vc);
        p++;
      }
      g_string_append_c (out, '"');
      state = 0;
    }
    /* Drop trailing '/' from emitted body so we can normalize
     * how the tag closes. */
    while (out->len > 0 &&
           (out->str[out->len - 1] == '/' ||
            g_ascii_isspace (out->str[out->len - 1])))
      g_string_truncate (out, out->len - 1);

    if (void_el || self_close)
      g_string_append (out, "/>");
    else {
      g_string_append_c (out, '>');
      g_ptr_array_add (stk, g_strdup (name));
    }
    i = end + 1;
  }

  for (gint s = (gint) stk->len - 1; s >= 0; s--)
    g_string_append_printf (out, "</%s>", (const char *) stk->pdata[s]);

  g_ptr_array_free (stk, TRUE);
  if (out_len) *out_len = out->len;
  return g_string_free (out, FALSE);
}

/* ── HTML → blocks walker ────────────────────────────────────── */

typedef struct {
  GListStore  *blocks;
  GHashTable  *anchors;
  gboolean     in_body;
  gboolean     accum_active;
  FwBlockKind  accum_kind;
  int          accum_level;
  GString     *accum;
  char        *pending_anchor;
} HtmlCtx;

static const char *
inline_open (const char *name)
{
  if (g_str_equal (name, "em") || g_str_equal (name, "i"))      return "<i>";
  if (g_str_equal (name, "strong") || g_str_equal (name, "b"))  return "<b>";
  if (g_str_equal (name, "code") || g_str_equal (name, "tt") ||
      g_str_equal (name, "kbd")  || g_str_equal (name, "samp")) return "<tt>";
  if (g_str_equal (name, "sub"))                                 return "<sub>";
  if (g_str_equal (name, "sup"))                                 return "<sup>";
  if (g_str_equal (name, "s") || g_str_equal (name, "strike") ||
      g_str_equal (name, "del"))                                 return "<s>";
  if (g_str_equal (name, "u") || g_str_equal (name, "ins") ||
      g_str_equal (name, "a"))                                   return "<u>";
  return NULL;
}
static const char *
inline_close (const char *name)
{
  if (g_str_equal (name, "em") || g_str_equal (name, "i"))      return "</i>";
  if (g_str_equal (name, "strong") || g_str_equal (name, "b"))  return "</b>";
  if (g_str_equal (name, "code") || g_str_equal (name, "tt") ||
      g_str_equal (name, "kbd")  || g_str_equal (name, "samp")) return "</tt>";
  if (g_str_equal (name, "sub"))                                 return "</sub>";
  if (g_str_equal (name, "sup"))                                 return "</sup>";
  if (g_str_equal (name, "s") || g_str_equal (name, "strike") ||
      g_str_equal (name, "del"))                                 return "</s>";
  if (g_str_equal (name, "u") || g_str_equal (name, "ins") ||
      g_str_equal (name, "a"))                                   return "</u>";
  return NULL;
}

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

static void
flush_accum (HtmlCtx *cc)
{
  if (!cc->accum_active) return;
  cc->accum_active = FALSE;
  while (cc->accum->len > 0 &&
         g_ascii_isspace (cc->accum->str[cc->accum->len - 1]))
    g_string_truncate (cc->accum, cc->accum->len - 1);
  if (cc->accum->len > 0) {
    FwBlock *b = fw_block_new (cc->accum_kind, cc->accum_level,
                               cc->accum->str, NULL,
                               cc->pending_anchor, 0);
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
}

static void
start_block (HtmlCtx *cc, FwBlockKind kind, int level,
             const char **attr_names, const char **attr_values)
{
  flush_accum (cc);
  cc->accum_active = TRUE;
  cc->accum_kind   = kind;
  cc->accum_level  = level;
  if (attr_names) {
    for (int i = 0; attr_names[i]; i++) {
      if (g_str_equal (attr_names[i], "id")) {
        g_clear_pointer (&cc->pending_anchor, g_free);
        cc->pending_anchor = g_strdup (attr_values[i]);
        break;
      }
    }
  }
}

static void
on_start (GMarkupParseContext *ctx G_GNUC_UNUSED,
          const gchar         *name,
          const gchar        **attr_names,
          const gchar        **attr_values,
          gpointer             user_data,
          GError             **error G_GNUC_UNUSED)
{
  HtmlCtx *cc = user_data;
  if (g_str_equal (name, "body")) { cc->in_body = TRUE; return; }
  if (!cc->in_body) return;

  /* MOBI page break / section markers — produce visual breaks. */
  if (g_str_equal (name, "mbp:pagebreak") ||
      g_str_equal (name, "pagebreak")) {
    flush_accum (cc);
    FwBlock *hr = fw_block_new (FW_BLOCK_HR, 0, NULL, NULL, NULL, 0);
    g_list_store_append (cc->blocks, hr);
    g_object_unref (hr);
    return;
  }
  if (g_str_equal (name, "mbp:section") || g_str_equal (name, "section")) {
    flush_accum (cc);
    const char *id = NULL;
    for (int i = 0; attr_names[i]; i++)
      if (g_str_equal (attr_names[i], "id")) id = attr_values[i];
    FwBlock *chap = fw_block_new (FW_BLOCK_CHAPTER, 0, NULL, NULL, id, 0);
    g_list_store_append (cc->blocks, chap);
    if (id) {
      guint pos = g_list_model_get_n_items (G_LIST_MODEL (cc->blocks)) - 1;
      g_hash_table_insert (cc->anchors, g_strdup (id),
                           GUINT_TO_POINTER (pos + 1));
    }
    g_object_unref (chap);
    return;
  }

  int hl = heading_level (name);
  if (hl > 0) {
    start_block (cc, FW_BLOCK_HEADING, hl, attr_names, attr_values);
    return;
  }
  if (g_str_equal (name, "p")) {
    start_block (cc, FW_BLOCK_PARAGRAPH, 0, attr_names, attr_values);
    return;
  }
  if (g_str_equal (name, "blockquote")) {
    start_block (cc, FW_BLOCK_BLOCKQUOTE, 0, attr_names, attr_values);
    return;
  }
  if (g_str_equal (name, "pre")) {
    start_block (cc, FW_BLOCK_CODE, 0, attr_names, attr_values);
    return;
  }
  if (g_str_equal (name, "li")) {
    start_block (cc, FW_BLOCK_PARAGRAPH, 0, attr_names, attr_values);
    g_string_append (cc->accum, "•  ");
    return;
  }
  if (g_str_equal (name, "hr")) {
    flush_accum (cc);
    FwBlock *hr = fw_block_new (FW_BLOCK_HR, 0, NULL, NULL, NULL, 0);
    g_list_store_append (cc->blocks, hr);
    g_object_unref (hr);
    return;
  }
  if (g_str_equal (name, "br")) {
    if (cc->accum_active) g_string_append_c (cc->accum, '\n');
    return;
  }

  /* MOBI <img recindex="N"> — N is the 1-based image record index.
   * The image has been pre-decoded into the document's images hash;
   * push an FW_BLOCK_IMAGE with image_id = recindex string. The view
   * resolves this through fw_reflow_document_get_image. */
  if (g_str_equal (name, "img")) {
    flush_accum (cc);
    const char *recindex = NULL;
    for (int i = 0; attr_names[i]; i++) {
      if (g_str_equal (attr_names[i], "recindex")) {
        recindex = attr_values[i];
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

  if (cc->accum_active) {
    const char *open = inline_open (name);
    if (open) g_string_append (cc->accum, open);
  }
}

static void
on_end (GMarkupParseContext *ctx G_GNUC_UNUSED,
        const gchar         *name,
        gpointer             user_data,
        GError             **error G_GNUC_UNUSED)
{
  HtmlCtx *cc = user_data;
  if (g_str_equal (name, "body")) { cc->in_body = FALSE; return; }
  if (!cc->in_body) return;
  if (heading_level (name) > 0 ||
      g_str_equal (name, "p") ||
      g_str_equal (name, "blockquote") ||
      g_str_equal (name, "pre") ||
      g_str_equal (name, "li")) {
    flush_accum (cc);
    return;
  }
  if (cc->accum_active) {
    const char *close = inline_close (name);
    if (close) g_string_append (cc->accum, close);
  }
}

static void
on_text (GMarkupParseContext *ctx G_GNUC_UNUSED,
         const gchar         *text,
         gsize                len,
         gpointer             user_data,
         GError             **error G_GNUC_UNUSED)
{
  HtmlCtx *cc = user_data;
  if (!cc->accum_active) return;
  for (gsize i = 0; i < len; i++) {
    char c = text[i];
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && cc->accum->len > 0 &&
        cc->accum->str[cc->accum->len - 1] == ' ') continue;
    if      (c == '<') g_string_append (cc->accum, "&lt;");
    else if (c == '>') g_string_append (cc->accum, "&gt;");
    else if (c == '&') g_string_append (cc->accum, "&amp;");
    else               g_string_append_c (cc->accum, c);
  }
}

#endif  /* end of old GMarkupParser walker */

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
  GString     *accum;
  char        *pending_anchor;

  /* Stack of inline Pango tag names that are currently open inside
   * the accumulator (e.g. {"i", "b"}). Tracked so that when an
   * inline span crosses a block-level boundary (an `<a>` wrapping
   * multiple `<p>`s, common in real HTML), we can auto-close on
   * flush and re-open on the next block. */
  GPtrArray   *open_inlines;
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
inline_open_tag (const char *name)
{
  if (g_str_equal (name, "em") || g_str_equal (name, "i"))      return "<i>";
  if (g_str_equal (name, "strong") || g_str_equal (name, "b"))  return "<b>";
  if (g_str_equal (name, "code") || g_str_equal (name, "tt") ||
      g_str_equal (name, "kbd")  || g_str_equal (name, "samp")) return "<tt>";
  if (g_str_equal (name, "sub"))                                 return "<sub>";
  if (g_str_equal (name, "sup"))                                 return "<sup>";
  if (g_str_equal (name, "s") || g_str_equal (name, "strike") ||
      g_str_equal (name, "del"))                                 return "<s>";
  if (g_str_equal (name, "u") || g_str_equal (name, "ins") ||
      g_str_equal (name, "a"))                                   return "<u>";
  return NULL;
}
static const char *
inline_close_tag (const char *name)
{
  if (g_str_equal (name, "em") || g_str_equal (name, "i"))      return "</i>";
  if (g_str_equal (name, "strong") || g_str_equal (name, "b"))  return "</b>";
  if (g_str_equal (name, "code") || g_str_equal (name, "tt") ||
      g_str_equal (name, "kbd")  || g_str_equal (name, "samp")) return "</tt>";
  if (g_str_equal (name, "sub"))                                 return "</sub>";
  if (g_str_equal (name, "sup"))                                 return "</sup>";
  if (g_str_equal (name, "s") || g_str_equal (name, "strike") ||
      g_str_equal (name, "del"))                                 return "</s>";
  if (g_str_equal (name, "u") || g_str_equal (name, "ins") ||
      g_str_equal (name, "a"))                                   return "</u>";
  return NULL;
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
                               cc->pending_anchor, 0);
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

static void
handle_element (WalkCtx *cc, xmlNode *n)
{
  const char *name = (const char *)n->name;
  /* Make a lowercase copy for case-insensitive matching. */
  g_autofree char *lname = g_ascii_strdown (name, -1);

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
  if (g_str_equal (lname, "li")) {
    start_accum (cc, FW_BLOCK_PARAGRAPH, 0, n);
    g_string_append (cc->accum, "•  ");
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

  /* libxml2 HTML mode — tolerant of malformed markup. The flags
   * RECOVER (continue on error) + NOERROR / NOWARNING (don't print
   * to stderr) match Foliate's DOMParser semantics. */
  htmlDocPtr xdoc = htmlReadMemory (
    m->body, (int) m->body_len,
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
  };
  walk_xml_node (&cc, xmlDocGetRootElement (xdoc));
  flush_accum (&cc);
  g_string_free (cc.accum, TRUE);
  g_clear_pointer (&cc.pending_anchor, g_free);
  g_ptr_array_free (cc.open_inlines, TRUE);
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
static GArray *mobi_search (FwReflowDocument *doc, const char *needle) {
  (void) doc; (void) needle; return NULL;
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
  iface->search                = mobi_search;
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
