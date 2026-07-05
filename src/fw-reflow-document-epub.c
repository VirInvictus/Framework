/* fw-reflow-document-epub.c — EPUB 2/3 reflow backend
 *
 * Reads the ZIP via libarchive (one streaming pass — every entry's
 * bytes get cached into a path → GBytes hash up front so subsequent
 * lookups are random-access). Parses META-INF/container.xml to find
 * the OPF rootfile; parses the OPF for manifest + spine + metadata;
 * builds the chapter TOC from the NCX (EPUB 2) or nav.xhtml (EPUB 3).
 * produce_html re-parses each spine chapter with libxml2's HTML parser,
 * rewrites <img> srcs to the framework-img:// scheme, strips scripts,
 * and stitches the bodies into one HTML document for the WebView.
 *
 * NOT in scope here:
 *   - Author CSS (deliberately dropped per design doc §3 "EPUB").
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document-epub.h"
#include "fw-reflow-html.h"

#include <archive.h>
#include <archive_entry.h>
#include <string.h>
#include <libxml/HTMLparser.h>
#include <libxml/HTMLtree.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

/* ── Type definition ──────────────────────────────────────────────── */

struct _FwReflowDocumentEpub {
  GObject       parent_instance;
  GListStore   *toc;        /* GListStore<FwReflowTocItem> */
  GHashTable   *metadata;   /* gchar* → gchar* */
  GHashTable   *zip;        /* gchar* (zip-path) → GBytes (owned) */
  char         *path;
  char         *opf_dir;    /* directory portion of OPF path within ZIP */
  gboolean      drm;        /* encrypted EPUB — produce_html emits a notice */

  /* WebView render path (produce_html).  Immutable for the lifetime of
   * the document — the URI scheme handler reads them from any thread. */
  GPtrArray    *spine_paths;        /* gchar* resolved zip paths in order */
  GHashTable   *zip_to_image_id;    /* gchar* zip-path → gchar* manifest-id (images only) */
  GHashTable   *image_bytes;        /* gchar* manifest-id → GBytes (owned) */
  char         *cover_image_id;     /* manifest id of the cover (NULL if none) */
};

static void fw_reflow_document_epub_iface_init (FwReflowDocumentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwReflowDocumentEpub,
                               fw_reflow_document_epub,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (FW_TYPE_REFLOW_DOCUMENT,
                                                      fw_reflow_document_epub_iface_init))

/* ── ZIP read into in-memory map ──────────────────────────────────── */

static gboolean
read_zip_entries (FwReflowDocumentEpub *self,
                  const char           *path,
                  GError              **error)
{
  struct archive *a = archive_read_new ();
  archive_read_support_format_zip (a);

  if (archive_read_open_filename (a, path, 16384) != ARCHIVE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "epub: cannot open archive: %s", archive_error_string (a));
    archive_read_free (a);
    return FALSE;
  }

  struct archive_entry *entry;
  while (archive_read_next_header (a, &entry) == ARCHIVE_OK) {
    if (archive_entry_filetype (entry) != AE_IFREG) {
      archive_read_data_skip (a);
      continue;
    }
    const char *name = archive_entry_pathname (entry);
    if (!name || !*name) {
      archive_read_data_skip (a);
      continue;
    }
    la_int64_t size = archive_entry_size (entry);
    if (size <= 0) {
      archive_read_data_skip (a);
      continue;
    }

    g_autofree gchar *buf = g_malloc (size);
    la_ssize_t got = archive_read_data (a, buf, size);
    if (got != size) {
      g_warning ("epub: short read on '%s' (%lld of %lld)", name,
                 (long long) got, (long long) size);
      continue;
    }

    GBytes *bytes = g_bytes_new_take (g_steal_pointer (&buf), size);
    g_hash_table_insert (self->zip, g_strdup (name), bytes);
  }

  archive_read_free (a);
  return TRUE;
}

/* ── Path resolution within the ZIP ───────────────────────────────── */

/* Resolve `ref` (which may be relative) against `base_dir` (which is
 * either empty or ends with '/') and normalize ./ and ../ segments.
 * Always returns a forward-slash path with no leading slash. */
static char *
resolve_zip_path (const char *base_dir, const char *ref)
{
  if (!ref || !*ref)
    return g_strdup ("");

  /* Drop fragment if any. */
  const char *frag = strchr (ref, '#');
  g_autofree char *clean_ref = frag ? g_strndup (ref, frag - ref) : g_strdup (ref);

  /* Absolute reference (starts with /) — drop the leading slash. */
  if (clean_ref[0] == '/')
    return g_strdup (clean_ref + 1);

  g_autofree char *combined =
    base_dir && *base_dir ? g_strconcat (base_dir, clean_ref, NULL)
                          : g_strdup (clean_ref);

  /* Normalize: split on '/', resolve . and .., rejoin. */
  GPtrArray *parts = g_ptr_array_new ();
  char *saveptr = NULL;
  for (char *p = strtok_r (combined, "/", &saveptr); p;
       p = strtok_r (NULL, "/", &saveptr)) {
    if (g_str_equal (p, "."))
      continue;
    if (g_str_equal (p, "..")) {
      if (parts->len > 0)
        g_ptr_array_remove_index (parts, parts->len - 1);
      continue;
    }
    g_ptr_array_add (parts, p);
  }

  GString *out = g_string_new (NULL);
  for (guint i = 0; i < parts->len; i++) {
    if (i > 0) g_string_append_c (out, '/');
    g_string_append (out, parts->pdata[i]);
  }
  g_ptr_array_free (parts, TRUE);
  return g_string_free (out, FALSE);
}

static char *
dirname_zip (const char *path)
{
  if (!path) return g_strdup ("");
  const char *slash = strrchr (path, '/');
  if (!slash)
    return g_strdup ("");
  return g_strndup (path, slash - path + 1);   /* trailing '/' kept */
}

/* ── container.xml parser ─────────────────────────────────────────── */

typedef struct { char *opf_path; } ContainerCtx;

static void
container_start (GMarkupParseContext *ctx G_GNUC_UNUSED,
                 const gchar         *name,
                 const gchar        **attr_names,
                 const gchar        **attr_values,
                 gpointer             user_data,
                 GError             **error G_GNUC_UNUSED)
{
  ContainerCtx *cc = user_data;
  if (!g_str_equal (name, "rootfile")) return;
  for (int i = 0; attr_names[i]; i++) {
    if (g_str_equal (attr_names[i], "full-path") && !cc->opf_path)
      cc->opf_path = g_strdup (attr_values[i]);
  }
}

static char *
find_opf_path (FwReflowDocumentEpub *self, GError **error)
{
  GBytes *cb = g_hash_table_lookup (self->zip, "META-INF/container.xml");
  if (!cb) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "epub: missing META-INF/container.xml");
    return NULL;
  }
  gsize len = 0;
  const char *data = g_bytes_get_data (cb, &len);

  static const GMarkupParser p = { .start_element = container_start };
  ContainerCtx cc = { 0 };
  GMarkupParseContext *gpc = g_markup_parse_context_new (&p, 0, &cc, NULL);
  gboolean ok = g_markup_parse_context_parse (gpc, data, len, error) &&
                g_markup_parse_context_end_parse (gpc, error);
  g_markup_parse_context_free (gpc);
  if (!ok) { g_free (cc.opf_path); return NULL; }
  if (!cc.opf_path)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "epub: container.xml has no rootfile full-path");
  return cc.opf_path;
}

/* ── OPF parser ───────────────────────────────────────────────────── */

typedef struct {
  /* Manifest: id → href (resolved) + media-type */
  GHashTable *manifest_href;   /* gchar* (id) → gchar* (resolved zip path) */
  GHashTable *manifest_type;   /* gchar* (id) → gchar* (media-type) */

  /* Spine: ordered list of idrefs */
  GPtrArray  *spine;           /* gchar* */

  /* NCX id (from spine toc="..."). */
  char       *toc_id;

  /* EPUB 3 nav.xhtml id (from manifest item with properties="nav"). */
  char       *nav_id;

  /* Cover image id. Populated by either:
   *   - EPUB 2: `<meta name="cover" content="X"/>` in <metadata>
   *   - EPUB 3: `<item properties="cover-image"/>` in <manifest> */
  char       *cover_id;

  /* Metadata accumulation */
  gboolean    in_metadata;
  GString    *meta_text;
  const char *meta_key;        /* current scalar key */
  GHashTable *out_metadata;    /* shared with backend */

  /* Where to resolve manifest hrefs */
  const char *opf_dir;
} OpfCtx;

static void
opf_start (GMarkupParseContext *ctx G_GNUC_UNUSED,
           const gchar         *name,
           const gchar        **attr_names,
           const gchar        **attr_values,
           gpointer             user_data,
           GError             **error G_GNUC_UNUSED)
{
  OpfCtx *oc = user_data;

  if (g_str_equal (name, "metadata")) { oc->in_metadata = TRUE; return; }

  if (g_str_equal (name, "spine")) {
    for (int i = 0; attr_names[i]; i++)
      if (g_str_equal (attr_names[i], "toc"))
        oc->toc_id = g_strdup (attr_values[i]);
    return;
  }

  if (g_str_equal (name, "itemref")) {
    for (int i = 0; attr_names[i]; i++)
      if (g_str_equal (attr_names[i], "idref"))
        g_ptr_array_add (oc->spine, g_strdup (attr_values[i]));
    return;
  }

  if (g_str_equal (name, "item")) {
    const char *id = NULL, *href = NULL, *mt = NULL, *props = NULL;
    for (int i = 0; attr_names[i]; i++) {
      if (g_str_equal (attr_names[i], "id"))         id    = attr_values[i];
      else if (g_str_equal (attr_names[i], "href"))  href  = attr_values[i];
      else if (g_str_equal (attr_names[i], "media-type")) mt = attr_values[i];
      else if (g_str_equal (attr_names[i], "properties")) props = attr_values[i];
    }
    if (id && href) {
      g_hash_table_insert (oc->manifest_href, g_strdup (id),
                           resolve_zip_path (oc->opf_dir, href));
      if (mt)
        g_hash_table_insert (oc->manifest_type, g_strdup (id), g_strdup (mt));
      /* EPUB 3: an item with properties containing "nav" is the
       * navigation document. There can be only one per spec. */
      if (props && !oc->nav_id) {
        if (g_strstr_len (props, -1, "nav"))
          oc->nav_id = g_strdup (id);
      }
      /* EPUB 3: cover-image property on manifest item. */
      if (props && !oc->cover_id) {
        if (g_strstr_len (props, -1, "cover-image"))
          oc->cover_id = g_strdup (id);
      }
    }
    return;
  }

  /* EPUB 2: <meta name="cover" content="cover-id"/> inside <metadata>.
   * The content value is the manifest id of the cover image item. */
  if (g_str_equal (name, "meta") && oc->in_metadata && !oc->cover_id) {
    const char *meta_name = NULL, *meta_content = NULL;
    for (int i = 0; attr_names[i]; i++) {
      if (g_str_equal (attr_names[i], "name"))    meta_name    = attr_values[i];
      else if (g_str_equal (attr_names[i], "content")) meta_content = attr_values[i];
    }
    if (meta_name && meta_content && g_ascii_strcasecmp (meta_name, "cover") == 0)
      oc->cover_id = g_strdup (meta_content);
  }

  /* Metadata scalars. Foliate captures the full Dublin Core set
   * plus EPUB 3 refines (language alternatives, contributor roles
   * via MARC relators, etc.). We capture the canonical text-content
   * subset; the exhaustive structured form is out of scope. */
  if (oc->in_metadata) {
    if (g_str_equal (name, "dc:title")       || g_str_equal (name, "title") ||
        g_str_equal (name, "dc:creator")     || g_str_equal (name, "creator") ||
        g_str_equal (name, "dc:contributor") || g_str_equal (name, "contributor") ||
        g_str_equal (name, "dc:publisher")   || g_str_equal (name, "publisher") ||
        g_str_equal (name, "dc:date")        || g_str_equal (name, "date") ||
        g_str_equal (name, "dc:language")    || g_str_equal (name, "language") ||
        g_str_equal (name, "dc:identifier")  || g_str_equal (name, "identifier") ||
        g_str_equal (name, "dc:description") || g_str_equal (name, "description") ||
        g_str_equal (name, "dc:subject")     || g_str_equal (name, "subject") ||
        g_str_equal (name, "dc:rights")      || g_str_equal (name, "rights") ||
        g_str_equal (name, "dc:source")      || g_str_equal (name, "source")) {
      if (!oc->meta_text) oc->meta_text = g_string_new (NULL);
      g_string_truncate (oc->meta_text, 0);
      oc->meta_key = name;
    }
  }
}

static void
opf_end (GMarkupParseContext *ctx G_GNUC_UNUSED,
         const gchar         *name,
         gpointer             user_data,
         GError             **error G_GNUC_UNUSED)
{
  OpfCtx *oc = user_data;

  if (g_str_equal (name, "metadata")) { oc->in_metadata = FALSE; return; }

  if (oc->meta_text && oc->meta_key && g_str_equal (name, oc->meta_key)) {
    /* Trim. */
    while (oc->meta_text->len > 0 &&
           g_ascii_isspace (oc->meta_text->str[oc->meta_text->len - 1]))
      g_string_truncate (oc->meta_text, oc->meta_text->len - 1);

    /* Map each dc:foo → canonical key. */
    const char *canonical = oc->meta_key;
    if      (g_str_has_suffix (oc->meta_key, "title"))       canonical = "title";
    else if (g_str_has_suffix (oc->meta_key, "creator"))     canonical = "author";
    else if (g_str_has_suffix (oc->meta_key, "contributor")) canonical = "contributor";
    else if (g_str_has_suffix (oc->meta_key, "language") ||
             g_str_has_suffix (oc->meta_key, "lang"))        canonical = "lang";
    else if (g_str_has_suffix (oc->meta_key, "publisher"))   canonical = "publisher";
    else if (g_str_has_suffix (oc->meta_key, "date"))        canonical = "date";
    else if (g_str_has_suffix (oc->meta_key, "identifier"))  canonical = "identifier";
    else if (g_str_has_suffix (oc->meta_key, "description")) canonical = "description";
    else if (g_str_has_suffix (oc->meta_key, "subject"))     canonical = "subject";
    else if (g_str_has_suffix (oc->meta_key, "rights"))      canonical = "rights";
    else if (g_str_has_suffix (oc->meta_key, "source"))      canonical = "source";

    /* First-write-wins for most fields; subjects accumulate
     * comma-separated since real EPUBs typically declare several. */
    if (oc->meta_text->len > 0) {
      if (g_str_equal (canonical, "subject")) {
        const char *prev = g_hash_table_lookup (oc->out_metadata, canonical);
        if (prev && *prev) {
          g_autofree char *combined =
            g_strdup_printf ("%s, %s", prev, oc->meta_text->str);
          g_hash_table_insert (oc->out_metadata,
                               g_strdup (canonical), g_steal_pointer (&combined));
        } else {
          g_hash_table_insert (oc->out_metadata,
                               g_strdup (canonical),
                               g_strdup (oc->meta_text->str));
        }
      } else if (!g_hash_table_contains (oc->out_metadata, canonical)) {
        g_hash_table_insert (oc->out_metadata,
                             g_strdup (canonical),
                             g_strdup (oc->meta_text->str));
      }
    }

    g_string_truncate (oc->meta_text, 0);
    oc->meta_key = NULL;
  }
}

static void
opf_text (GMarkupParseContext *ctx G_GNUC_UNUSED,
          const gchar         *text,
          gsize                len,
          gpointer             user_data,
          GError             **error G_GNUC_UNUSED)
{
  OpfCtx *oc = user_data;
  if (oc->meta_text && oc->meta_key)
    g_string_append_len (oc->meta_text, text, len);
}


/* ── NCX TOC parser (EPUB 2 / 3 fallback) ─────────────────────────── */

typedef struct {
  GListStore *toc;
  const char *ncx_dir;        /* directory of the NCX file, for src resolution */
  /* state for current navPoint */
  GString    *cur_text;
  gboolean    in_navlabel;
  gboolean    in_text;
  char       *cur_src;        /* resolved chapter#frag */
} NcxCtx;

static void
ncx_start (GMarkupParseContext *ctx G_GNUC_UNUSED,
           const gchar         *name,
           const gchar        **attr_names,
           const gchar        **attr_values,
           gpointer             user_data,
           GError             **error G_GNUC_UNUSED)
{
  NcxCtx *nc = user_data;
  if (g_str_equal (name, "navLabel")) { nc->in_navlabel = TRUE; return; }
  if (nc->in_navlabel && g_str_equal (name, "text")) {
    if (!nc->cur_text) nc->cur_text = g_string_new (NULL);
    g_string_truncate (nc->cur_text, 0);
    nc->in_text = TRUE;
    return;
  }
  if (g_str_equal (name, "content")) {
    for (int i = 0; attr_names[i]; i++) {
      if (g_str_equal (attr_names[i], "src")) {
        const char *s = attr_values[i];
        const char *frag = strchr (s, '#');
        g_autofree char *base =
          frag ? g_strndup (s, frag - s) : g_strdup (s);
        g_autofree char *resolved = resolve_zip_path (nc->ncx_dir, base);
        g_free (nc->cur_src);   /* defensive; normally already NULL after a flush */
        if (frag)
          nc->cur_src = g_strconcat (resolved, frag, NULL);
        else
          nc->cur_src = g_steal_pointer (&resolved);
        break;
      }
    }
    /* Flush this navPoint's entry here, at its <content>. In NCX a
     * navPoint's navLabel and content precede its child navPoints, so
     * cur_text/cur_src hold THIS entry at this point. Flushing at
     * navPoint-end instead dropped every parent (section) entry, because
     * a child's navLabel/content overwrote the parent's state first.
     * Document order == reading order, so parents land before children.
     * cur_src is consumed (cleared) so it can't be flushed twice. */
    if (nc->cur_text && nc->cur_text->len > 0 && nc->cur_src) {
      while (nc->cur_text->len > 0 &&
             g_ascii_isspace (nc->cur_text->str[nc->cur_text->len - 1]))
        g_string_truncate (nc->cur_text, nc->cur_text->len - 1);
      FwReflowTocItem *item =
        fw_reflow_toc_item_new (nc->cur_text->str, nc->cur_src);
      g_list_store_append (nc->toc, item);
      g_object_unref (item);
    }
    if (nc->cur_text) g_string_truncate (nc->cur_text, 0);
    g_clear_pointer (&nc->cur_src, g_free);
    return;
  }
}

static void
ncx_end (GMarkupParseContext *ctx G_GNUC_UNUSED,
         const gchar         *name,
         gpointer             user_data,
         GError             **error G_GNUC_UNUSED)
{
  NcxCtx *nc = user_data;
  if (g_str_equal (name, "navLabel")) { nc->in_navlabel = FALSE; return; }
  if (g_str_equal (name, "text") && nc->in_text) {
    nc->in_text = FALSE;
    return;
  }
  if (g_str_equal (name, "navPoint")) {
    /* The entry was already flushed at <content> (see ncx_start). Just
     * drop any leftover state from a navPoint that carried no content. */
    if (nc->cur_text) g_string_truncate (nc->cur_text, 0);
    g_clear_pointer (&nc->cur_src, g_free);
  }
}

static void
ncx_text (GMarkupParseContext *ctx G_GNUC_UNUSED,
          const gchar         *text,
          gsize                len,
          gpointer             user_data,
          GError             **error G_GNUC_UNUSED)
{
  NcxCtx *nc = user_data;
  if (nc->in_text && nc->cur_text)
    g_string_append_len (nc->cur_text, text, len);
}

static void
parse_ncx (FwReflowDocumentEpub *self, const char *ncx_path, GBytes *bytes)
{
  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);

  g_autofree char *dir = dirname_zip (ncx_path);
  NcxCtx nc = { .toc = self->toc, .ncx_dir = dir };

  static const GMarkupParser p = {
    .start_element = ncx_start,
    .end_element   = ncx_end,
    .text          = ncx_text,
  };
  g_autoptr (GError) e = NULL;
  GMarkupParseContext *gpc =
    g_markup_parse_context_new (&p, G_MARKUP_TREAT_CDATA_AS_TEXT, &nc, NULL);
  gboolean ok =
    g_markup_parse_context_parse (gpc, data, len, &e) &&
    g_markup_parse_context_end_parse (gpc, &e);
  if (!ok)
    g_warning ("epub: NCX parse failed: %s", e ? e->message : "(unknown)");

  g_markup_parse_context_free (gpc);
  if (nc.cur_text) g_string_free (nc.cur_text, TRUE);
  g_clear_pointer (&nc.cur_src, g_free);
}

/* ── EPUB 3 nav.xhtml TOC parser ───────────────────────────────
 *
 * Foliate's `parseNav` walks a `<nav epub:type="toc">` element
 * inside the navigation document, extracting `<a href text>` pairs
 * from its `<ol>` tree. The href targets are URI-relative to the
 * nav doc's directory; we resolve them through `resolve_zip_path`
 * to match the CHAPTER anchors emitted by the spine walk.
 *
 * Parsed into the same `self->toc` GListStore the NCX walker uses,
 * so the sidebar UX is identical regardless of which TOC source
 * the EPUB carries.
 */

static gboolean
nav_node_is_toc_nav (xmlNode *n)
{
  if (n->type != XML_ELEMENT_NODE) return FALSE;
  if (g_ascii_strcasecmp ((const char *)n->name, "nav") != 0) return FALSE;
  for (xmlAttr *a = n->properties; a; a = a->next) {
    if ((g_ascii_strcasecmp ((const char *)a->name, "type") == 0 ||
         g_ascii_strcasecmp ((const char *)a->name, "epub:type") == 0)
        && a->children) {
      const char *v = (const char *)a->children->content;
      if (v && g_strstr_len (v, -1, "toc"))
        return TRUE;
    }
  }
  return FALSE;
}

static char *
xml_node_text_recursive (xmlNode *n)
{
  GString *out = g_string_new (NULL);
  for (xmlNode *c = n; c; c = c->next) {
    if (c->type == XML_TEXT_NODE && c->content) {
      g_string_append (out, (const char *)c->content);
    } else if (c->type == XML_ELEMENT_NODE) {
      g_autofree char *inner = xml_node_text_recursive (c->children);
      if (inner) g_string_append (out, inner);
    }
  }
  /* Squash internal whitespace to single space. */
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

static void
nav_walk_toc_a_links (FwReflowDocumentEpub *self, xmlNode *node,
                      const char *nav_dir)
{
  for (xmlNode *n = node; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    if (g_ascii_strcasecmp ((const char *)n->name, "a") == 0) {
      const char *href = NULL;
      for (xmlAttr *a = n->properties; a; a = a->next) {
        if (g_ascii_strcasecmp ((const char *)a->name, "href") == 0
            && a->children) {
          href = (const char *)a->children->content;
          break;
        }
      }
      if (href) {
        const char *frag = strchr (href, '#');
        g_autofree char *base =
          frag ? g_strndup (href, frag - href) : g_strdup (href);
        g_autofree char *resolved = resolve_zip_path (nav_dir, base);
        g_autofree char *anchor = frag
          ? g_strconcat (resolved, frag, NULL)
          : g_strdup (resolved);

        g_autofree char *label = xml_node_text_recursive (n->children);
        if (label && *label && anchor && *anchor) {
          FwReflowTocItem *item = fw_reflow_toc_item_new (label, anchor);
          g_list_store_append (self->toc, item);
          g_object_unref (item);
        }
      }
      continue;  /* don't recurse into <a> children — already extracted text */
    }
    nav_walk_toc_a_links (self, n->children, nav_dir);
  }
}

static void
nav_walk_for_toc_nav (FwReflowDocumentEpub *self, xmlNode *node,
                      const char *nav_dir)
{
  for (xmlNode *n = node; n; n = n->next) {
    if (n->type == XML_ELEMENT_NODE && nav_node_is_toc_nav (n)) {
      nav_walk_toc_a_links (self, n->children, nav_dir);
      return;
    }
    if (n->children)
      nav_walk_for_toc_nav (self, n->children, nav_dir);
  }
}

static void
parse_nav_xhtml (FwReflowDocumentEpub *self,
                 const char           *nav_path,
                 GBytes               *bytes)
{
  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);

  htmlDocPtr xdoc = htmlReadMemory (
    data, (int) len,
    NULL, "UTF-8",
    HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING |
    HTML_PARSE_NONET    | HTML_PARSE_NOBLANKS);
  if (!xdoc) return;

  g_autofree char *nav_dir = dirname_zip (nav_path);
  nav_walk_for_toc_nav (self, xmlDocGetRootElement (xdoc), nav_dir);

  xmlFreeDoc (xdoc);
}

/* ── Encryption classification ────────────────────────────────────── */

/* EPUB OCF resource obfuscation (embedded-font mangling) uses one of
 * these two algorithms. Both scramble only font resources; the text
 * content stays readable. Anything else in encryption.xml is real
 * content encryption (Adobe ADEPT, Apple FairPlay, …) that we can't
 * render. See https://www.w3.org/publishing/epub32/epub-ocf.html and
 * .foliate-js/epub.js (the two obfuscation algorithm URIs). */
static gboolean
enc_algorithm_is_obfuscation (const char *algo)
{
  return algo &&
    (g_strcmp0 (algo, "http://www.idpf.org/2008/embedding") == 0 ||
     g_strcmp0 (algo, "http://ns.adobe.com/pdf/enc#RC") == 0);
}

static void
enc_scan_node (xmlNodePtr node, gboolean *has_drm)
{
  for (xmlNodePtr n = node; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE)
      continue;
    if (xmlStrcasecmp (n->name, BAD_CAST "EncryptionMethod") == 0) {
      xmlChar *algo = xmlGetProp (n, BAD_CAST "Algorithm");
      if (!enc_algorithm_is_obfuscation ((const char *) algo))
        *has_drm = TRUE;
      if (algo) xmlFree (algo);
    }
    enc_scan_node (n->children, has_drm);
  }
}

/* Classify META-INF/encryption.xml: TRUE only when some EncryptedData
 * uses a non-obfuscation algorithm (real DRM). Font-obfuscation-only
 * manifests — very common in DRM-free retail EPUBs — are NOT DRM, so
 * the book opens and reflows normally. (The obfuscated fonts simply
 * fall back to the bundled reading fonts: publisher CSS/fonts aren't
 * served yet, so font de-obfuscation is queued with the Publisher-CSS
 * phase in roadmap.md.) An unparseable manifest is treated as DRM —
 * we can't verify it's benign. */
static gboolean
epub_encryption_is_drm (GBytes *enc)
{
  gsize len = 0;
  const char *data = g_bytes_get_data (enc, &len);
  xmlDocPtr d = xmlReadMemory (data, (int) len, NULL, "UTF-8",
                               XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
                               XML_PARSE_NONET);
  if (!d)
    return TRUE;

  gboolean has_drm = FALSE;
  enc_scan_node (xmlDocGetRootElement (d), &has_drm);
  xmlFreeDoc (d);
  return has_drm;
}

/* ── Top-level open() ─────────────────────────────────────────────── */

static gboolean
epub_open (FwReflowDocument *doc, const char *path, GError **error)
{
  FwReflowDocumentEpub *self = FW_REFLOW_DOCUMENT_EPUB (doc);

  if (!read_zip_entries (self, path, error))
    return FALSE;

  /* Encryption classification — META-INF/encryption.xml alone does NOT
   * mean DRM: it also marks EPUB OCF font obfuscation, which is common
   * in DRM-free retail EPUBs and leaves the text fully readable. Only
   * treat the book as DRM when a non-obfuscation algorithm is present
   * (see epub_encryption_is_drm). Font-obfuscation-only books fall
   * through and open normally. */
  GBytes *enc = g_hash_table_lookup (self->zip, "META-INF/encryption.xml");
  if (enc && epub_encryption_is_drm (enc)) {
    g_hash_table_insert (self->metadata, g_strdup ("title"),
                         g_strdup ("DRM-protected EPUB"));
    g_hash_table_insert (self->metadata, g_strdup ("format"),
                         g_strdup ("EPUB (encrypted)"));
    self->drm = TRUE;   /* produce_html emits the notice */
    self->path = g_strdup (path);
    return TRUE;
  }

  /* Container → OPF path */
  g_autofree char *opf_path = find_opf_path (self, error);
  if (!opf_path)
    return FALSE;
  self->opf_dir = dirname_zip (opf_path);

  GBytes *opf_bytes = g_hash_table_lookup (self->zip, opf_path);
  if (!opf_bytes) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "epub: OPF '%s' missing from archive", opf_path);
    return FALSE;
  }

  OpfCtx oc = {
    .manifest_href = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, g_free),
    .manifest_type = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, g_free),
    .spine         = g_ptr_array_new_with_free_func (g_free),
    .out_metadata  = self->metadata,
    .opf_dir       = self->opf_dir,
  };

  static const GMarkupParser opf_parser = {
    .start_element = opf_start,
    .end_element   = opf_end,
    .text          = opf_text,
  };
  {
    gsize olen = 0;
    const char *odata = g_bytes_get_data (opf_bytes, &olen);
    GMarkupParseContext *gpc =
      g_markup_parse_context_new (&opf_parser, 0, &oc, NULL);
    gboolean ok =
      g_markup_parse_context_parse (gpc, odata, olen, error) &&
      g_markup_parse_context_end_parse (gpc, error);
    g_markup_parse_context_free (gpc);
    if (oc.meta_text) g_string_free (oc.meta_text, TRUE);
    if (!ok) {
      g_hash_table_destroy (oc.manifest_href);
      g_hash_table_destroy (oc.manifest_type);
      g_ptr_array_free (oc.spine, TRUE);
      g_free (oc.toc_id);
      g_free (oc.nav_id);
      g_free (oc.cover_id);
      return FALSE;
    }
  }

  /* Record the cover manifest id so produce_html can prepend a cover
   * section before the spine. */
  if (oc.cover_id)
    self->cover_image_id = g_strdup (oc.cover_id);

  /* Record the resolved spine paths in order so produce_html can stitch
   * the chapters without re-walking the OPF. */
  for (guint i = 0; i < oc.spine->len; i++) {
    const char *idref = oc.spine->pdata[i];
    const char *href  = g_hash_table_lookup (oc.manifest_href, idref);
    if (!href) {
      g_warning ("epub: spine idref '%s' not in manifest", idref);
      continue;
    }
    if (!g_hash_table_contains (self->zip, href)) {
      g_warning ("epub: chapter '%s' missing from archive", href);
      continue;
    }
    g_ptr_array_add (self->spine_paths, g_strdup (href));
  }

  /* TOC: prefer NCX when present (EPUB 2 + most EPUB 3 books still
   * ship one for back-compat). Fall back to EPUB 3 nav.xhtml when
   * NCX is absent or empty. Some pure-EPUB-3 books only carry a
   * nav doc — `parseNav` is the canonical path foliate takes. */
  if (oc.toc_id) {
    const char *ncx_href = g_hash_table_lookup (oc.manifest_href, oc.toc_id);
    if (ncx_href) {
      GBytes *nb = g_hash_table_lookup (self->zip, ncx_href);
      if (nb)
        parse_ncx (self, ncx_href, nb);
    }
  }
  if (g_list_model_get_n_items (G_LIST_MODEL (self->toc)) == 0 && oc.nav_id) {
    const char *nav_href = g_hash_table_lookup (oc.manifest_href, oc.nav_id);
    if (nav_href) {
      GBytes *nb = g_hash_table_lookup (self->zip, nav_href);
      if (nb)
        parse_nav_xhtml (self, nav_href, nb);
    }
  }

  /* Build the produce_html image tables: zip-path → manifest-id (for
   * rewriting <img src> while stitching) and image_bytes keyed by
   * manifest-id (the table fw_webview_load_html receives; WebKit
   * decodes the bytes itself). */
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, oc.manifest_type);
  while (g_hash_table_iter_next (&it, &k, &v)) {
    const char *id = k;
    const char *mt = v;
    if (!g_str_has_prefix (mt, "image/"))
      continue;
    const char *href = g_hash_table_lookup (oc.manifest_href, id);
    if (!href) continue;
    GBytes *b = g_hash_table_lookup (self->zip, href);
    if (!b) continue;
    g_hash_table_insert (self->zip_to_image_id, g_strdup (href), g_strdup (id));
    g_hash_table_insert (self->image_bytes,     g_strdup (id),   g_bytes_ref (b));
  }

  /* Fallback metadata. */
  if (!g_hash_table_contains (self->metadata, "title")) {
    g_autofree char *basename = g_path_get_basename (path);
    g_hash_table_insert (self->metadata, g_strdup ("title"), g_strdup (basename));
  }
  g_hash_table_insert (self->metadata, g_strdup ("format"),
                       g_strdup ("EPUB"));

  self->path = g_strdup (path);

  g_hash_table_destroy (oc.manifest_href);
  g_hash_table_destroy (oc.manifest_type);
  g_ptr_array_free (oc.spine, TRUE);
  g_free (oc.toc_id);
  g_free (oc.nav_id);
  g_free (oc.cover_id);

  return TRUE;
}

static void epub_close (FwReflowDocument *doc) {
  FwReflowDocumentEpub *self = FW_REFLOW_DOCUMENT_EPUB (doc);
  if (self->toc) g_list_store_remove_all (self->toc);
}

static GListModel *epub_get_toc (FwReflowDocument *doc) {
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_EPUB (doc)->toc);
}
static GHashTable *epub_get_metadata (FwReflowDocument *doc) {
  FwReflowDocumentEpub *self = FW_REFLOW_DOCUMENT_EPUB (doc);
  return self->metadata ? g_hash_table_ref (self->metadata) : NULL;
}

/* ── produce_html: stitch spine into one HTML document ──────────────────
 *
 * Concatenates every spine chapter's <body> into a single HTML document
 * the WebKitWebView can load in one call.  Image src attributes get
 * resolved relative to their chapter's zip-directory and rewritten to
 * `framework-img://<doc-id>/<manifest-id>`, the URI scheme fw-webview.c
 * registers globally.  <script> and <link rel="stylesheet"> get
 * stripped — the former for safety, the latter because we don't ship
 * the EPUB's CSS resource graph through the URI scheme yet (Phase 17.x
 * will add framework-css:; until then a small built-in reading
 * stylesheet picks up the slack).
 *
 * Resolution rules mirror what a browser does:
 *   - src starting with "/"  → archive-absolute, strip leading /
 *   - else                   → resolved relative to the chapter's dir,
 *                              with `.` and `..` segments normalised.
 *
 * The returned image table keys on manifest id (URI-safe XML
 * name-tokens) and shares the existing per-image GBytes refs from
 * self->image_bytes.  No copy. */

/* Resolve an EPUB <img> to its manifest image id: src is a zip path
 * relative to the chapter, looked up through zip_to_image_id. Shared
 * subtree pass (fw_reflow_html_process) does the rewrite + script strip. */
typedef struct {
  const char *chapter_path;
  GHashTable *zip_to_image_id;
} EpubImgCtx;

static char *
epub_img_resolver (xmlNodePtr img, gpointer user_data)
{
  EpubImgCtx *c = user_data;
  xmlChar *src = xmlGetProp (img, BAD_CAST "src");
  if (!src) return NULL;
  g_autofree char *chapter_dir = dirname_zip (c->chapter_path);
  g_autofree char *resolved =
    resolve_zip_path (chapter_dir, (const char *) src);
  xmlFree (src);
  if (!resolved || !*resolved) return NULL;
  const char *image_id = g_hash_table_lookup (c->zip_to_image_id, resolved);
  return image_id ? g_strdup (image_id) : NULL;
}

static gboolean
epub_produce_html (FwReflowDocument  *doc,
                   const char        *doc_id,
                   char             **out_html,
                   GHashTable       **out_images,
                   GError           **error)
{
  FwReflowDocumentEpub *self = FW_REFLOW_DOCUMENT_EPUB (doc);

  /* Encrypted EPUB: render a plain explanation rather than failing the
   * open (which would fall through to MuPDF and show garbage). */
  if (self->drm) {
    GString *m = g_string_new ("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
                               "<title>DRM-protected EPUB</title><style>");
    g_string_append (m, fw_reflow_reading_css ());
    g_string_append (m, "</style></head><body><section data-spine=\"0\">"
                        "<h1>This book is DRM-protected.</h1>"
                        "<p>Framework does not support DRM-encrypted EPUBs. "
                        "Convert via Calibre's DeDRM plugin, or open with a "
                        "DRM-aware reader.</p>"
                        "</section></body></html>");
    if (out_images) *out_images = NULL;
    *out_html = g_string_free (m, FALSE);
    return TRUE;
  }

  if (!self->spine_paths || self->spine_paths->len == 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "epub: spine is empty");
    return FALSE;
  }

  GString *out = g_string_new (NULL);
  const char *title = self->metadata
                        ? g_hash_table_lookup (self->metadata, "title")
                        : NULL;
  const char *lang  = self->metadata
                        ? g_hash_table_lookup (self->metadata, "lang")
                        : NULL;

  g_string_append (out, "<!DOCTYPE html>\n<html");
  if (lang && *lang) g_string_append_printf (out, " lang=\"%s\"", lang);
  g_string_append (out, "><head><meta charset=\"utf-8\">");
  if (title && *title) {
    g_autofree char *esc = g_markup_escape_text (title, -1);
    g_string_append_printf (out, "<title>%s</title>", esc);
  }
  g_string_append (out, "<style>");
  g_string_append (out, fw_reflow_reading_css ());
  g_string_append (out, "</style></head><body>");

  /* Cover image (when declared via EPUB 2 <meta name="cover" /> or
   * EPUB 3 `properties="cover-image"`).  We only emit a standalone
   * cover section when the manifest id resolves to an actual image —
   * many EPUB 2 files point the cover meta at a cover XHTML page that
   * already sits at the head of the spine, and emitting a broken-img
   * placeholder before that page would leave the first viewport blank.
   * Skipping the emission lets the spine walk render the cover XHTML
   * naturally. */
  if (self->cover_image_id &&
      self->image_bytes &&
      g_hash_table_contains (self->image_bytes, self->cover_image_id)) {
    g_string_append_printf (out,
      "<section class=\"cover\" id=\"cover\">"
        "<img src=\"framework-img://%s/%s\" alt=\"Cover\">"
      "</section>",
      doc_id, self->cover_image_id);
  }

  /* For each spine entry, parse with libxml2's HTML parser, walk to
   * <body>, rewrite img srcs, strip scripts/stylesheet links, dump
   * the children into a <section> wrapper.  htmlNodeDump on each
   * child avoids the wrapper-document boilerplate that
   * htmlDocDumpMemory would emit. */
  xmlBufferPtr buf = xmlBufferCreate ();
  for (guint i = 0; i < self->spine_paths->len; i++) {
    const char *zip_path = self->spine_paths->pdata[i];
    GBytes *bytes = g_hash_table_lookup (self->zip, zip_path);
    if (!bytes) continue;
    gsize n;
    const char *p = g_bytes_get_data (bytes, &n);

    /* Explicit UTF-8: EPUB chapters are UTF-8 per spec, and without
     * the encoding hint libxml2's auto-detection falls back to
     * ISO-8859-1, which means every multi-byte UTF-8 sequence comes
     * out doubly-encoded once htmlNodeDump re-serialises it
     * (apostrophes turn into Mojibake like "â\x80\x99"). */
    htmlDocPtr d = htmlReadMemory (
      p, (int) n, NULL, "UTF-8",
      HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
      HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (!d) continue;

    xmlNodePtr root = xmlDocGetRootElement (d);
    xmlNodePtr body = root ? fw_reflow_html_find_body (root) : NULL;
    if (body) {
      EpubImgCtx ictx = {
        .chapter_path    = zip_path,
        .zip_to_image_id = self->zip_to_image_id,
      };
      fw_reflow_html_process (body, doc_id, epub_img_resolver, &ictx);

      g_string_append_printf (out, "<section data-spine=\"%u\" id=\"spine-%u\">", i, i);
      for (xmlNodePtr c = body->children; c; c = c->next) {
        xmlBufferEmpty (buf);
        htmlNodeDump (buf, d, c);
        g_string_append (out, (const char *) xmlBufferContent (buf));
      }
      g_string_append (out, "</section>");
    }
    xmlFreeDoc (d);
  }
  xmlBufferFree (buf);

  g_string_append (out, "</body></html>");

  if (out_images)
    *out_images = self->image_bytes
                    ? g_hash_table_ref (self->image_bytes)
                    : NULL;
  *out_html = g_string_free (out, FALSE);
  return TRUE;
}

static void
fw_reflow_document_epub_iface_init (FwReflowDocumentInterface *iface)
{
  iface->open                  = epub_open;
  iface->close                 = epub_close;
  iface->get_toc               = epub_get_toc;
  iface->get_metadata          = epub_get_metadata;
  iface->produce_html          = epub_produce_html;
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_reflow_document_epub_finalize (GObject *object)
{
  FwReflowDocumentEpub *self = FW_REFLOW_DOCUMENT_EPUB (object);
  g_clear_object (&self->toc);
  g_clear_pointer (&self->metadata, g_hash_table_unref);
  g_clear_pointer (&self->zip,      g_hash_table_unref);
  g_clear_pointer (&self->path,     g_free);
  g_clear_pointer (&self->opf_dir,  g_free);
  g_clear_pointer (&self->spine_paths,     g_ptr_array_unref);
  g_clear_pointer (&self->zip_to_image_id, g_hash_table_unref);
  g_clear_pointer (&self->image_bytes,     g_hash_table_unref);
  g_clear_pointer (&self->cover_image_id,  g_free);
  G_OBJECT_CLASS (fw_reflow_document_epub_parent_class)->finalize (object);
}

static void
fw_reflow_document_epub_class_init (FwReflowDocumentEpubClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fw_reflow_document_epub_finalize;
}

static void
fw_reflow_document_epub_init (FwReflowDocumentEpub *self)
{
  self->toc      = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
  self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, g_free);
  self->zip      = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free,
                                          (GDestroyNotify) g_bytes_unref);
  self->spine_paths     = g_ptr_array_new_with_free_func (g_free);
  self->zip_to_image_id = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, g_free);
  self->image_bytes     = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free,
                                                  (GDestroyNotify) g_bytes_unref);
}

FwReflowDocumentEpub *
fw_reflow_document_epub_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_DOCUMENT_EPUB, NULL);
}
