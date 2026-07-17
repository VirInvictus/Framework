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
#include "fw-reflow-html.h"

#include <string.h>
#include <stdlib.h>
#include <libxml/HTMLparser.h>
#include <libxml/HTMLtree.h>
#include <libxml/tree.h>

struct _FwReflowDocumentMobi {
  GObject       parent_instance;
  GListStore   *toc;
  GHashTable   *metadata;
  /* WebView (produce_html) path, retained from mobi_open: the
   * marker-injected HTML body and the raw image bytes by recindex. */
  GHashTable   *image_bytes;/* gchar* (recindex string) → GBytes */
  char         *html_body;  /* marker-injected UTF-8 HTML (owned) */
  gsize         html_len;
  guint         cover_recindex; /* EXTH cover; 0 = none */
  GArray       *frag_offsets;   /* KF8 fid → body offset (owned ref;
                                 * NULL for KF7), for kindle:pos links */
  /* KF8 publisher-CSS flows, keyed by the /res/ path a rewritten
   * kindle:flow link points at ("flow/<i>.css" → GBytes). Backs
   * mobi_get_resources / the framework-img://.../res/ scheme. NULL for
   * KF7 and for KF8 without publisher flows. */
  GHashTable   *resources;
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

/* ── KF8 kindle:pos links ────────────────────────────────────────
 *
 * KF8 internal links are `<a href="kindle:pos:fid:XXXX:off:YYYYYYYYYY">`
 * with base-32 fields (foliate mobi.js parsePosURI); the target byte
 * offset in the spliced body is frag_offsets[fid] + off. Targets get
 * "ncx_<offset>" markers injected at open (merged with the NCX TOC
 * positions — same prefix, so ids unify), and the hrefs are rewritten
 * to "#ncx_<offset>" at produce time. */

static gboolean
kf8_parse_pos (const char *href, guint32 *out_fid, guint32 *out_off)
{
  const char *p = strstr (href, "kindle:pos:fid:");
  if (!p)
    return FALSE;
  p += strlen ("kindle:pos:fid:");
  char *end = NULL;
  guint64 fid = g_ascii_strtoull (p, &end, 32);
  if (end == p || !g_str_has_prefix (end, ":off:"))
    return FALSE;
  p = end + strlen (":off:");
  guint64 off = g_ascii_strtoull (p, &end, 32);
  if (end == p || fid > G_MAXUINT32 || off > G_MAXUINT32)
    return FALSE;
  *out_fid = (guint32) fid;
  *out_off = (guint32) off;
  return TRUE;
}

static gboolean
kf8_pos_to_abs (GArray *frag_offsets, guint32 fid, guint32 off,
                guint32 *out_abs)
{
  if (!frag_offsets || fid >= frag_offsets->len)
    return FALSE;
  guint32 base = g_array_index (frag_offsets, guint32, fid);
  if (base == G_MAXUINT32)
    return FALSE;
  *out_abs = base + off;
  return TRUE;
}

/* Scan the spliced body for kindle:pos link targets so markers can be
 * injected at their offsets alongside the NCX ones. */
static void
kf8_collect_pos_targets (const char *body, gsize len,
                         GArray *frag_offsets, GArray *positions)
{
  static const char needle[] = "kindle:pos:fid:";
  const char *end = body + len;
  const char *p = body;
  while ((p = g_strstr_len (p, end - p, needle))) {
    guint32 fid, off, abs;
    if (kf8_parse_pos (p, &fid, &off) &&
        kf8_pos_to_abs (frag_offsets, fid, off, &abs) &&
        abs <= len)
      g_array_append_val (positions, abs);
    p += sizeof (needle) - 1;
  }
}

/* Sort ascending and drop duplicates — the marker injector requires
 * ascending positions, and duplicate ids would be invalid HTML. */
static void
positions_sort_unique (GArray *positions)
{
  if (positions->len < 2) {
    g_array_sort (positions, cmp_guint32);
    return;
  }
  g_array_sort (positions, cmp_guint32);
  guint w = 1;
  for (guint i = 1; i < positions->len; i++) {
    guint32 v = g_array_index (positions, guint32, i);
    if (v != g_array_index (positions, guint32, w - 1))
      g_array_index (positions, guint32, w++) = v;
  }
  g_array_set_size (positions, w);
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
        /* Numeric-normalize: markers are "filepos_<parsed value>", and
         * kindlegen zero-pads the attribute ("filepos=0000034567"). */
        guint64 v = g_ascii_strtoull (filepos, NULL, 10);
        g_autofree char *anchor = g_strdup_printf ("filepos_%u", (guint32) v);
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
          guint64 v = g_ascii_strtoull (filepos, NULL, 10);
          g_autofree char *anchor = g_strdup_printf ("filepos_%u", (guint32) v);
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

  /* Steal the raw image bytes too, for the WebView produce_html path. */
  if (m->image_bytes) {
    g_clear_pointer (&self->image_bytes, g_hash_table_unref);
    self->image_bytes = m->image_bytes;
    m->image_bytes = NULL;
  }

  /* KF8 publisher-CSS flows → the /res/ table. Flow i (i >= 1) is served
   * as "flow/<i>.css"; produce_html rewrites each in-body kindle:flow:<i>
   * stylesheet link to framework-img://<doc>/res/flow/<i>.css, and the
   * webview scheme handler serves it (extension-first MIME → text/css).
   * Registering every non-empty flow is harmless: only the ones actually
   * linked get fetched, and the reading CSS keeps its !important
   * sovereignty over theme and typography (mirrors EPUB v0.79). */
  g_clear_pointer (&self->resources, g_hash_table_unref);
  if (m->flows) {
    for (guint i = 0; i < m->flows->len; i++) {
      GBytes *b = g_ptr_array_index (m->flows, i);
      if (!b) continue;
      if (!self->resources)
        self->resources = g_hash_table_new_full (
          g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
      g_hash_table_insert (self->resources,
                           g_strdup_printf ("flow/%u.css", i),
                           g_bytes_ref (b));
    }
  }

  /* If a cover image was identified via EXTH-201, push it as the
   * first block — flagged FW_BLOCK_FLAG_COVER so FwReflowView
   * gives it a full-viewport page. */
  self->cover_recindex = m->cover_recindex;

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
  if (kf8_toc || (m->is_kf8 && m->frag_offsets)) {
    positions = g_array_new (FALSE, FALSE, sizeof (guint32));
    if (kf8_toc) {
      for (guint i = 0; i < m->toc->len; i++) {
        guint32 o = g_array_index (m->toc, FwMobiTocEntry, i).body_offset;
        g_array_append_val (positions, o);
      }
    }
    /* In-body kindle:pos link targets get markers too, under the same
     * "ncx_" prefix, so internal links land somewhere real. */
    if (m->frag_offsets)
      kf8_collect_pos_targets (m->body, m->body_len,
                               m->frag_offsets, positions);
    positions_sort_unique (positions);       /* injector needs ascending */
    marker_prefix = "ncx_";
  } else {
    positions = mobi_collect_filepos (m->body, m->body_len);  /* sorted, unique */
    marker_prefix = "filepos_";
  }

  /* Retain the KF8 fragment table for kindle:pos href rewriting in
   * produce_html. */
  g_clear_pointer (&self->frag_offsets, g_array_unref);
  if (m->frag_offsets) {
    self->frag_offsets = m->frag_offsets;
    m->frag_offsets = NULL;
  }

  gsize body_len2 = 0;
  char *body2 = mobi_inject_markers (
    m->body, m->body_len, positions, marker_prefix, &body_len2);
  /* Retain the marker-injected body for the WebView produce_html path
   * (carries the same id anchors the TOC resolves against). Owned by
   * self now; freed in dispose. */
  g_free (self->html_body);
  self->html_body = body2;
  self->html_len  = body_len2;

  /* libxml2 HTML mode — tolerant of malformed markup. The flags
   * RECOVER (continue on error) + NOERROR / NOWARNING (don't print
   * to stderr) match Foliate's DOMParser semantics. */
  htmlDocPtr xdoc = htmlReadMemory (
    body2, (int) body_len2,
    NULL, "UTF-8",
    HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING |
    HTML_PARSE_NONET    | HTML_PARSE_NOBLANKS);
  if (!xdoc) {
    g_warning ("mobi: htmlReadMemory returned NULL");
    fw_mobi_parsed_free (m);
    self->path = g_strdup (path);
    return TRUE;
  }

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
  (void) doc;
}
static GListModel *mobi_get_toc (FwReflowDocument *doc) {
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_MOBI (doc)->toc);
}
static GHashTable *mobi_get_metadata (FwReflowDocument *doc) {
  FwReflowDocumentMobi *self = FW_REFLOW_DOCUMENT_MOBI (doc);
  return self->metadata ? g_hash_table_ref (self->metadata) : NULL;
}
static GHashTable *mobi_get_resources (FwReflowDocument *doc) {
  FwReflowDocumentMobi *self = FW_REFLOW_DOCUMENT_MOBI (doc);
  return self->resources ? g_hash_table_ref (self->resources) : NULL;
}

/* Resolve a MOBI <img> to its image-bytes key. Two reference forms,
 * both ported from foliate-js mobi.js:
 *   - KF7: `recindex="00001"` (zero-padded decimal). Key = the parsed
 *     number (recindex N maps to loadResource(N-1); the parser keys the
 *     same record N, so key == Number(recindex)).
 *   - KF8/AZW3: `src="kindle:embed:000K?mime=..."` where the id is
 *     base-32; key = parseInt(id, 32) (embed id maps to loadResource(id-1),
 *     keyed id by the parser).
 * Rewrite only when we hold those bytes (else leave src for WebKit). */
static char *
mobi_img_resolver (xmlNodePtr img, gpointer user_data)
{
  GHashTable *image_bytes = user_data;
  long n = -1;

  xmlChar *ri = xmlGetProp (img, BAD_CAST "recindex");
  if (ri) {
    n = strtol ((const char *) ri, NULL, 10);
    xmlFree (ri);
  } else {
    xmlChar *src = xmlGetProp (img, BAD_CAST "src");
    if (src) {
      const char *embed = strstr ((const char *) src, "kindle:embed:");
      if (embed)
        n = strtol (embed + strlen ("kindle:embed:"), NULL, 32);
      xmlFree (src);
    }
  }
  if (n <= 0) return NULL;

  char *id = g_strdup_printf ("%ld", n);
  if (image_bytes && !g_hash_table_contains (image_bytes, id)) {
    g_free (id);
    return NULL;
  }
  return id;
}

/* Give internal links a working href. KF7 anchors carry the target as
 * a `filepos` attribute, never an href — rewrite to "#filepos_<N>"
 * (numeric-normalized: markers use the parsed value, attrs are often
 * zero-padded). KF8 uses kindle:pos hrefs — rewrite to "#ncx_<abs>"
 * via the fragment table, or drop the href when unresolvable so a dead
 * link degrades to plain text instead of a blocked navigation. */
static void
mobi_rewrite_links (xmlNodePtr node, GArray *frag_offsets)
{
  for (xmlNodePtr n = node; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE)
      continue;
    if (xmlStrcasecmp (n->name, BAD_CAST "a") == 0) {
      xmlChar *fp = xmlGetProp (n, BAD_CAST "filepos");
      if (fp && *fp) {
        guint64 v = g_ascii_strtoull ((const char *) fp, NULL, 10);
        g_autofree char *href = g_strdup_printf ("#filepos_%u", (guint32) v);
        xmlSetProp (n, BAD_CAST "href", BAD_CAST href);
      } else {
        xmlChar *href = xmlGetProp (n, BAD_CAST "href");
        if (href && strstr ((const char *) href, "kindle:")) {
          guint32 fid, off, abs;
          if (kf8_parse_pos ((const char *) href, &fid, &off) &&
              kf8_pos_to_abs (frag_offsets, fid, off, &abs)) {
            g_autofree char *nh = g_strdup_printf ("#ncx_%u", abs);
            xmlSetProp (n, BAD_CAST "href", BAD_CAST nh);
          } else {
            xmlUnsetProp (n, BAD_CAST "href");
          }
        }
        if (href) xmlFree (href);
      }
      if (fp) xmlFree (fp);
    }
    if (n->children)
      mobi_rewrite_links (n->children, frag_offsets);
  }
}

/* Collect KF8 publisher stylesheets from the parsed document so they can
 * be hoisted into the stitched shell's <head>. The body-only stitch
 * below never sees the per-section <head>s where these live, which is
 * why KF8 rendered without publisher CSS before. Two forms:
 *   - <link rel="stylesheet" href="kindle:flow:<id>?mime=text/css">: the
 *     base-32 id is the flow index; emit a /res/flow/<i>.css link that
 *     resolves against self->resources, deduped by flow id.
 *   - inline <style>: emitted verbatim.
 * Both carry class="fw-pub" so fw_webview_set_publisher_styles can flip
 * them live, matching the EPUB backend. */
static void
mobi_collect_head_css (xmlNodePtr node, const char *doc_id,
                       GString *head_out, GHashTable *seen)
{
  for (xmlNodePtr n = node; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) {
      continue;
    }
    if (xmlStrcasecmp (n->name, BAD_CAST "link") == 0) {
      xmlChar *rel  = xmlGetProp (n, BAD_CAST "rel");
      xmlChar *href = xmlGetProp (n, BAD_CAST "href");
      if (rel && href
          && g_ascii_strcasecmp ((const char *) rel, "stylesheet") == 0
          && g_str_has_prefix ((const char *) href, "kindle:flow:")) {
        const char *idp = (const char *) href + strlen ("kindle:flow:");
        char idbuf[16];
        gsize k = 0;
        while (idp[k] && idp[k] != '?' && k < sizeof idbuf - 1)
          idbuf[k] = idp[k], k++;
        idbuf[k] = '\0';
        guint fid = (guint) g_ascii_strtoull (idbuf, NULL, 32);
        if (fid > 0
            && !g_hash_table_contains (seen, GUINT_TO_POINTER (fid))) {
          g_hash_table_add (seen, GUINT_TO_POINTER (fid));
          g_string_append_printf (head_out,
            "<link class=\"fw-pub\" rel=\"stylesheet\" "
            "href=\"framework-img://%s/res/flow/%u.css\">",
            doc_id, fid);
        }
      }
      if (rel)  xmlFree (rel);
      if (href) xmlFree (href);
    } else if (xmlStrcasecmp (n->name, BAD_CAST "style") == 0) {
      xmlChar *txt = xmlNodeGetContent (n);
      if (txt && *txt) {
        g_string_append (head_out, "<style class=\"fw-pub\">");
        g_string_append (head_out, (const char *) txt);
        g_string_append (head_out, "</style>");
      }
      if (txt) xmlFree (txt);
    }
    if (n->children)
      mobi_collect_head_css (n->children, doc_id, head_out, seen);
  }
}

/* WebView path: emit the marker-injected MOBI body as one stitched HTML
 * document with the shared reading CSS, img recindex rewritten to the
 * framework-img: scheme, scripts stripped. Returns the raw image bytes
 * as the image table. */
static gboolean
mobi_produce_html (FwReflowDocument *doc, const char *doc_id,
                   char **out_html, GHashTable **out_images, GError **error)
{
  FwReflowDocumentMobi *self = FW_REFLOW_DOCUMENT_MOBI (doc);
  if (!self->html_body || self->html_len == 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "mobi: empty body");
    return FALSE;
  }

  const char *title = self->metadata
                        ? g_hash_table_lookup (self->metadata, "title") : NULL;
  const char *lang  = self->metadata
                        ? g_hash_table_lookup (self->metadata, "lang") : NULL;

  /* Parse first: collect the publisher (flow) stylesheets from the
   * document's <head>s and build the body section, so the shell can
   * order publisher CSS ahead of the reading stylesheet. */
  g_autoptr (GString) pubcss = g_string_new (NULL);
  g_autoptr (GString) sec = g_string_new (NULL);
  htmlDocPtr d = htmlReadMemory (
    self->html_body, (int) self->html_len, NULL, "UTF-8",
    HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING |
    HTML_PARSE_NONET);
  if (d) {
    xmlNodePtr root = xmlDocGetRootElement (d);
    if (root) {
      g_autoptr (GHashTable) seen =
        g_hash_table_new (g_direct_hash, g_direct_equal);
      mobi_collect_head_css (root, doc_id, pubcss, seen);
    }
    xmlNodePtr body = root ? fw_reflow_html_find_body (root) : NULL;
    if (body) {
      fw_reflow_html_process (body, doc_id, mobi_img_resolver,
                              self->image_bytes);
      mobi_rewrite_links (body->children, self->frag_offsets);
      g_string_append (sec, "<section data-spine=\"0\" id=\"spine-0\">");
      xmlBufferPtr buf = xmlBufferCreate ();
      for (xmlNodePtr c = body->children; c; c = c->next) {
        xmlBufferEmpty (buf);
        htmlNodeDump (buf, d, c);
        g_string_append (sec, (const char *) xmlBufferContent (buf));
      }
      xmlBufferFree (buf);
      g_string_append (sec, "</section>");
    }
    xmlFreeDoc (d);
  }

  GString *out = g_string_new ("<!DOCTYPE html>\n<html");
  if (lang && *lang) g_string_append_printf (out, " lang=\"%s\"", lang);
  g_string_append (out, "><head><meta charset=\"utf-8\">");
  if (title && *title) {
    g_autofree char *esc = g_markup_escape_text (title, -1);
    g_string_append_printf (out, "<title>%s</title>", esc);
  }
  /* Publisher (flow) CSS first, then the reading stylesheet: its
   * !important body rules keep theme/typography sovereignty. The
   * fw-reading-css id lets fw_webview_set_publisher_styles leave it
   * enabled while it toggles the fw-pub sheets (matches EPUB v0.79). */
  if (pubcss->len)
    g_string_append (out, pubcss->str);
  g_string_append (out, "<style id=\"fw-reading-css\">");
  g_string_append (out, fw_reflow_reading_css ());
  g_string_append (out, "</style></head><body>");

  /* Synthetic cover: only when the EXTH cover image exists and the body
   * doesn't already reference it (avoids a doubled cover). */
  if (self->cover_recindex > 0 && self->image_bytes) {
    g_autofree char *cover_id = g_strdup_printf ("%u", self->cover_recindex);
    if (g_hash_table_contains (self->image_bytes, cover_id)) {
      g_autofree char *needle =
        g_strdup_printf ("framework-img://%s/%s\"", doc_id, cover_id);
      if (!strstr (sec->str, needle))
        g_string_append_printf (out,
          "<section class=\"cover\" id=\"cover\">"
            "<img src=\"framework-img://%s/%s\" alt=\"Cover\">"
          "</section>",
          doc_id, cover_id);
    }
  }

  g_string_append (out, sec->str);
  g_string_append (out, "</body></html>");

  if (out_images)
    *out_images = self->image_bytes ? g_hash_table_ref (self->image_bytes) : NULL;
  *out_html = g_string_free (out, FALSE);
  return TRUE;
}

static void
fw_reflow_document_mobi_iface_init (FwReflowDocumentInterface *iface)
{
  iface->open                  = mobi_open;
  iface->close                 = mobi_close;
  iface->get_toc               = mobi_get_toc;
  iface->get_metadata          = mobi_get_metadata;
  iface->get_resources         = mobi_get_resources;
  iface->produce_html          = mobi_produce_html;
}

static void
fw_reflow_document_mobi_finalize (GObject *object)
{
  FwReflowDocumentMobi *self = FW_REFLOW_DOCUMENT_MOBI (object);
  g_clear_object (&self->toc);
  g_clear_pointer (&self->metadata, g_hash_table_unref);
  g_clear_pointer (&self->image_bytes, g_hash_table_unref);
  g_clear_pointer (&self->html_body, g_free);
  g_clear_pointer (&self->frag_offsets, g_array_unref);
  g_clear_pointer (&self->resources, g_hash_table_unref);
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
  self->toc      = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
  self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

FwReflowDocumentMobi *
fw_reflow_document_mobi_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_DOCUMENT_MOBI, NULL);
}
