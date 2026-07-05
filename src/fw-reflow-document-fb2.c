/* fw-reflow-document-fb2.c — FB2 (FictionBook 2) reflow backend
 *
 * libxml2-based port from `.foliate-js/fb2.js`. produce_html transforms
 * the parsed FB2 tree to HTML for the WebView, applying foliate's logical
 * element mapping:
 *
 *   <section>            → <section id=…> (scroll anchor)
 *   <title>/<subtitle>   → <hN> by section depth
 *   <p>                  → <p>
 *   <epigraph>, <cite>,
 *   <poem>, <annotation> → <blockquote>
 *   <empty-line/>        → <hr>
 *   <image l:href="#X">  → <img framework-img://…/X> (raw <binary id=X> bytes)
 *
 * A separate pre-pass extracts metadata, decodes <binary> images to raw
 * bytes, and builds the chapter TOC from the section tree.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document-fb2.h"
#include "fw-reflow-html.h"

#include <string.h>
#include <archive.h>
#include <archive_entry.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

struct _FwReflowDocumentFb2 {
  GObject       parent_instance;
  GListStore   *toc;        /* GListStore<FwReflowTocItem> */
  GHashTable   *metadata;   /* gchar* → gchar* */
  /* Raw decoded image bytes (id → GBytes), served to the WebView via
   * the framework-img:// scheme. */
  GHashTable   *image_bytes;
  /* Parsed FB2 tree, retained for produce_html (the WebView path
   * transforms FB2 XML to HTML); freed in finalize. */
  xmlDocPtr     xdoc;
  char         *path;
};

static void fw_reflow_document_fb2_iface_init (FwReflowDocumentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwReflowDocumentFb2,
                               fw_reflow_document_fb2,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (FW_TYPE_REFLOW_DOCUMENT,
                                                      fw_reflow_document_fb2_iface_init))

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

/* ── Forward decls ────────────────────────────────────────────── */

static void fb2_metadata_set  (FwReflowDocumentFb2 *self,
                               const char *key, const char *val);

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
fb2_walk_metadata (FwReflowDocumentFb2 *self, xmlNode *root)
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
      fb2_metadata_set (self, "title", t);
    } else if (g_str_equal (cname, "lang") || g_str_equal (cname, "src-lang")) {
      g_autofree char *t = fb2_node_text (c);
      fb2_metadata_set (self, "lang", t);
    } else if (g_str_equal (cname, "annotation")) {
      g_autofree char *t = fb2_node_text (c);
      fb2_metadata_set (self, "annotation", t);
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
        fb2_metadata_set (self, "author", au->str);
    }
  }
}

/* ── Binary walker ─────────────────────────────────────────────── */

static void
fb2_walk_binaries (FwReflowDocumentFb2 *self, xmlNode *root)
{
  /* `<binary id="..." content-type="image/...">BASE64</binary>` —
   * decode base64 → raw bytes for the WebView (WebKit decodes them). */
  for (xmlNode *n = root; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    g_autofree char *lname = g_ascii_strdown ((const char *)n->name, -1);
    if (!g_str_equal (lname, "binary")) {
      if (n->children) fb2_walk_binaries (self, n->children);
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

    g_hash_table_insert (self->image_bytes, g_strdup (id),
                         g_bytes_new (bytes, out_len));
  }
}

/* ── TOC builder ───────────────────────────────────────────────── */

/* Build the chapter TOC straight from the parsed tree: each <section>
 * with an id contributes an entry labelled by its first <title> child's
 * text. The section ids match what produce_html emits as `<section id>`
 * anchors, so the sidebar can navigate to them. This replaces the TOC
 * side-effect the old block walk carried. */
static void
fb2_collect_toc (FwReflowDocumentFb2 *self, xmlNode *node)
{
  for (xmlNode *n = node; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    g_autofree char *lname = g_ascii_strdown ((const char *)n->name, -1);

    if (g_str_equal (lname, "section")) {
      const char *id = NULL;
      for (xmlAttr *a = n->properties; a; a = a->next) {
        if (g_ascii_strcasecmp ((const char *)a->name, "id") == 0 && a->children) {
          id = (const char *)a->children->content;
          break;
        }
      }
      if (id && *id) {
        for (xmlNode *c = n->children; c; c = c->next) {
          if (c->type != XML_ELEMENT_NODE) continue;
          g_autofree char *cl = g_ascii_strdown ((const char *)c->name, -1);
          if (g_str_equal (cl, "title")) {
            g_autofree char *t = fb2_node_text (c);
            if (t && *t) {
              FwReflowTocItem *item = fw_reflow_toc_item_new (t, id);
              g_list_store_append (self->toc, item);
              g_object_unref (item);
            }
            break;
          }
        }
      }
    }
    /* Descend (sections nest; body wraps them). */
    if (n->children) fb2_collect_toc (self, n->children);
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

  /* Metadata (title, author, language, annotation). */
  fb2_walk_metadata (self, root->children);

  /* <binary> elements → raw image bytes, so every <image l:href="#X"/>
   * resolves at render time even when the binary appears after the body. */
  fb2_walk_binaries (self, root->children);

  /* Chapter TOC, straight from the section tree. */
  fb2_collect_toc (self, root->children);

  /* Retain the parsed tree for produce_html (WebView path). */
  self->xdoc = xdoc;

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
  if (self->toc) g_list_store_remove_all (self->toc);
}

static GListModel *
fb2_get_toc (FwReflowDocument *doc)
{
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_FB2 (doc)->toc);
}

static GHashTable *
fb2_get_metadata (FwReflowDocument *doc)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  return self->metadata ? g_hash_table_ref (self->metadata) : NULL;
}

/* ── produce_html: transform the FB2 tree to HTML for the WebView ── */

typedef struct {
  const char *doc_id;
  GHashTable *image_bytes;
} Fb2HtmlCtx;

/* FB2 inline element → HTML tag, or NULL to pass children through
 * (style / span / a and friends carry no styling we honour here). */
static const char *
fb2_html_inline_tag (const char *l)
{
  if (g_str_equal (l, "emphasis"))      return "em";
  if (g_str_equal (l, "strong"))        return "strong";
  if (g_str_equal (l, "code"))          return "code";
  if (g_str_equal (l, "sub"))           return "sub";
  if (g_str_equal (l, "sup"))           return "sup";
  if (g_str_equal (l, "strikethrough")) return "s";
  return NULL;
}

static const char *
fb2_attr_id (xmlNode *n)
{
  for (xmlAttr *a = n->properties; a; a = a->next)
    if (g_ascii_strcasecmp ((const char *) a->name, "id") == 0 && a->children)
      return (const char *) a->children->content;
  return NULL;
}

static void fb2_emit_node     (GString *out, xmlNode *n, Fb2HtmlCtx *ctx, int depth);
static void fb2_emit_children (GString *out, xmlNode *n, Fb2HtmlCtx *ctx, int depth);

static void
fb2_emit_escaped (GString *out, const char *s)
{
  g_autofree char *e = g_markup_escape_text (s, -1);
  g_string_append (out, e);
}

/* <title>/<subtitle> wrap <p> lines; render them inline, joined by <br>,
 * so the heading is one <hN> rather than nested paragraphs. */
static void
fb2_emit_heading_inner (GString *out, xmlNode *n, Fb2HtmlCtx *ctx)
{
  gboolean first = TRUE;
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type == XML_TEXT_NODE && c->content) {
      fb2_emit_escaped (out, (const char *) c->content);
    } else if (c->type == XML_ELEMENT_NODE) {
      g_autofree char *l = g_ascii_strdown ((const char *) c->name, -1);
      if (g_str_equal (l, "p")) {
        if (!first) g_string_append (out, "<br>");
        fb2_emit_children (out, c, ctx, 0);
        first = FALSE;
      } else if (g_str_equal (l, "empty-line")) {
        g_string_append (out, "<br>");
      } else {
        fb2_emit_node (out, c, ctx, 0);
      }
    }
  }
}

static void
fb2_emit_node (GString *out, xmlNode *n, Fb2HtmlCtx *ctx, int depth)
{
  if (n->type == XML_TEXT_NODE) {
    if (n->content) fb2_emit_escaped (out, (const char *) n->content);
    return;
  }
  if (n->type != XML_ELEMENT_NODE) return;
  g_autofree char *l = g_ascii_strdown ((const char *) n->name, -1);

  if (g_str_equal (l, "section")) {
    const char *id = fb2_attr_id (n);
    g_string_append (out, "<section");
    if (id && *id) {
      g_autofree char *e = g_markup_escape_text (id, -1);
      g_string_append_printf (out, " id=\"%s\"", e);
    }
    g_string_append_c (out, '>');
    fb2_emit_children (out, n, ctx, depth + 1);
    g_string_append (out, "</section>");
    return;
  }
  if (g_str_equal (l, "title") || g_str_equal (l, "subtitle")) {
    int lvl = CLAMP (g_str_equal (l, "subtitle") ? depth + 1 : depth, 1, 6);
    g_string_append_printf (out, "<h%d>", lvl);
    fb2_emit_heading_inner (out, n, ctx);
    g_string_append_printf (out, "</h%d>", lvl);
    return;
  }
  if (g_str_equal (l, "p") || g_str_equal (l, "text-author")) {
    g_string_append (out, "<p>");
    fb2_emit_children (out, n, ctx, depth);
    g_string_append (out, "</p>");
    return;
  }
  if (g_str_equal (l, "empty-line")) { g_string_append (out, "<hr>"); return; }
  if (g_str_equal (l, "v")) {
    fb2_emit_children (out, n, ctx, depth);
    g_string_append (out, "<br>");
    return;
  }
  if (g_str_equal (l, "epigraph") || g_str_equal (l, "cite") ||
      g_str_equal (l, "poem") || g_str_equal (l, "annotation")) {
    g_string_append (out, "<blockquote>");
    fb2_emit_children (out, n, ctx, depth);
    g_string_append (out, "</blockquote>");
    return;
  }
  if (g_str_equal (l, "stanza")) {           /* inside a poem blockquote */
    fb2_emit_children (out, n, ctx, depth);
    return;
  }
  if (g_str_equal (l, "image")) {
    const char *href = fb2_get_href (n);
    const char *id = href ? (href[0] == '#' ? href + 1 : href) : NULL;
    if (id && ctx->image_bytes && g_hash_table_contains (ctx->image_bytes, id)) {
      g_autofree char *e = g_markup_escape_text (id, -1);
      g_string_append_printf (out, "<img src=\"framework-img://%s/%s\">",
                              ctx->doc_id, e);
    }
    return;
  }

  const char *itag = fb2_html_inline_tag (l);
  if (itag) {
    g_string_append_printf (out, "<%s>", itag);
    fb2_emit_children (out, n, ctx, depth);
    g_string_append_printf (out, "</%s>", itag);
    return;
  }
  /* Unknown / passthrough (style, span, a, ...): keep the text. */
  fb2_emit_children (out, n, ctx, depth);
}

static void
fb2_emit_children (GString *out, xmlNode *n, Fb2HtmlCtx *ctx, int depth)
{
  for (xmlNode *c = n->children; c; c = c->next)
    fb2_emit_node (out, c, ctx, depth);
}

static gboolean
fb2_produce_html (FwReflowDocument *doc, const char *doc_id,
                  char **out_html, GHashTable **out_images, GError **error)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  xmlNode *root = self->xdoc ? xmlDocGetRootElement (self->xdoc) : NULL;
  if (!root) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "fb2: no parsed tree");
    return FALSE;
  }

  const char *title = g_hash_table_lookup (self->metadata, "title");
  const char *lang  = g_hash_table_lookup (self->metadata, "lang");

  GString *out = g_string_new ("<!DOCTYPE html>\n<html");
  if (lang && *lang) {
    g_autofree char *e = g_markup_escape_text (lang, -1);
    g_string_append_printf (out, " lang=\"%s\"", e);
  }
  g_string_append (out, "><head><meta charset=\"utf-8\">");
  if (title && *title) {
    g_autofree char *e = g_markup_escape_text (title, -1);
    g_string_append_printf (out, "<title>%s</title>", e);
  }
  g_string_append (out, "<style>");
  g_string_append (out, fw_reflow_reading_css ());
  g_string_append (out, "</style></head><body>");

  Fb2HtmlCtx ctx = { .doc_id = doc_id, .image_bytes = self->image_bytes };
  /* FB2 may carry several <body> elements (main text + footnote bodies);
   * emit each as its own spine section. */
  for (xmlNode *n = root->children; n; n = n->next) {
    if (n->type == XML_ELEMENT_NODE &&
        g_ascii_strcasecmp ((const char *) n->name, "body") == 0) {
      g_string_append (out, "<section data-spine=\"0\">");
      fb2_emit_children (out, n, &ctx, 0);
      g_string_append (out, "</section>");
    }
  }
  g_string_append (out, "</body></html>");

  if (out_images)
    *out_images = self->image_bytes ? g_hash_table_ref (self->image_bytes) : NULL;
  *out_html = g_string_free (out, FALSE);
  return TRUE;
}

static void
fw_reflow_document_fb2_iface_init (FwReflowDocumentInterface *iface)
{
  iface->open                  = fb2_open;
  iface->close                 = fb2_close;
  iface->get_toc               = fb2_get_toc;
  iface->get_metadata          = fb2_get_metadata;
  iface->produce_html          = fb2_produce_html;
}

/* ── GObject boilerplate ──────────────────────────────────────── */

static void
fw_reflow_document_fb2_finalize (GObject *object)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (object);
  g_clear_object (&self->toc);
  g_clear_pointer (&self->metadata, g_hash_table_unref);
  g_clear_pointer (&self->image_bytes, g_hash_table_unref);
  g_clear_pointer (&self->xdoc,     xmlFreeDoc);
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
  self->toc      = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
  self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, g_free);
  self->image_bytes = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify) g_bytes_unref);
}

FwReflowDocumentFb2 *
fw_reflow_document_fb2_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_DOCUMENT_FB2, NULL);
}
