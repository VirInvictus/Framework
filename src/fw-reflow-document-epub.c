/* fw-reflow-document-epub.c — EPUB 2/3 reflow backend
 *
 * Reads the ZIP via libarchive (one streaming pass — every entry's
 * bytes get cached into a path → GBytes hash up front so subsequent
 * lookups are random-access). Parses META-INF/container.xml to find
 * the OPF rootfile; parses the OPF for manifest + spine + metadata;
 * walks the spine in order, parsing each XHTML through GMarkupParser
 * into FwBlocks. Image manifest items are decoded to GdkTextures and
 * keyed by both their resolved ZIP path and their manifest id.
 *
 * NOT in scope here:
 *   - Author CSS (deliberately dropped per design doc §3 "EPUB").
 *   - Nested list / table layout (real LIST / LIST_ITEM model is
 *     deferred; <li> renders as a "• "-prefixed paragraph).
 *   - Tolerant HTML for malformed XHTML (current parser is strict
 *     GMarkupParser; chapter files that fail to parse are skipped
 *     with a warning and the rest of the book opens).
 *   - DRM detection (encrypted EPUBs will surface as parse errors).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document-epub.h"

#include <archive.h>
#include <archive_entry.h>
#include <string.h>

/* ── Type definition ──────────────────────────────────────────────── */

struct _FwReflowDocumentEpub {
  GObject       parent_instance;
  GListStore   *blocks;     /* GListStore<FwBlock> */
  GListStore   *toc;        /* GListStore<FwReflowTocItem> */
  GHashTable   *metadata;   /* gchar* → gchar* */
  GHashTable   *images;     /* gchar* (zip-path or manifest-id) → GdkTexture */
  GHashTable   *anchors;    /* gchar* (anchor_id) → guint position+1 */
  GHashTable   *zip;        /* gchar* (zip-path) → GBytes (owned) */
  char         *path;
  char         *opf_dir;    /* directory portion of OPF path within ZIP */
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
    const char *id = NULL, *href = NULL, *mt = NULL;
    for (int i = 0; attr_names[i]; i++) {
      if (g_str_equal (attr_names[i], "id"))         id   = attr_values[i];
      else if (g_str_equal (attr_names[i], "href"))  href = attr_values[i];
      else if (g_str_equal (attr_names[i], "media-type")) mt = attr_values[i];
    }
    if (id && href) {
      g_hash_table_insert (oc->manifest_href, g_strdup (id),
                           resolve_zip_path (oc->opf_dir, href));
      if (mt)
        g_hash_table_insert (oc->manifest_type, g_strdup (id), g_strdup (mt));
    }
    return;
  }

  /* Metadata scalars: dc:title, dc:creator, dc:language, etc. */
  if (oc->in_metadata) {
    if (g_str_equal (name, "dc:title")    || g_str_equal (name, "title") ||
        g_str_equal (name, "dc:creator")  || g_str_equal (name, "creator") ||
        g_str_equal (name, "dc:language") || g_str_equal (name, "language") ||
        g_str_equal (name, "dc:publisher")|| g_str_equal (name, "publisher") ||
        g_str_equal (name, "dc:date")     || g_str_equal (name, "date")) {
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

    /* Map dc:title → title, dc:creator → author, etc. */
    const char *canonical = oc->meta_key;
    if      (g_str_has_suffix (oc->meta_key, "title"))     canonical = "title";
    else if (g_str_has_suffix (oc->meta_key, "creator"))   canonical = "author";
    else if (g_str_has_suffix (oc->meta_key, "language") ||
             g_str_has_suffix (oc->meta_key, "lang"))      canonical = "lang";
    else if (g_str_has_suffix (oc->meta_key, "publisher")) canonical = "publisher";
    else if (g_str_has_suffix (oc->meta_key, "date"))      canonical = "date";

    /* First-write-wins: many EPUBs declare dc:creator multiple times. */
    if (oc->meta_text->len > 0 &&
        !g_hash_table_contains (oc->out_metadata, canonical))
      g_hash_table_insert (oc->out_metadata,
                           g_strdup (canonical),
                           g_strdup (oc->meta_text->str));

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

/* ── XHTML chapter parser ─────────────────────────────────────────── */

typedef struct {
  GListStore  *blocks;       /* shared with backend */
  GHashTable  *anchors;      /* shared */
  /* OPF/spine context */
  const char  *chapter_path; /* resolved zip path of the current chapter */
  /* Block accumulator */
  gboolean     accum_active;
  FwBlockKind  accum_kind;
  int          accum_level;
  GString     *accum;
  char        *pending_anchor;
  /* Body scope — XHTML can have <head><body> */
  gboolean     in_body;
  /* Whether we already emitted a CHAPTER marker for this chapter
   * (we do so on first content block, not blindly at chapter open
   * — keeps the CHAPTER right above the content). */
  gboolean     chapter_marker_pushed;
} XhtmlCtx;

static const char *
xhtml_inline_open (const char *name)
{
  if (g_str_equal (name, "em") || g_str_equal (name, "i"))    return "<i>";
  if (g_str_equal (name, "strong") || g_str_equal (name, "b"))return "<b>";
  if (g_str_equal (name, "code") || g_str_equal (name, "tt") ||
      g_str_equal (name, "kbd")  || g_str_equal (name, "samp"))return "<tt>";
  if (g_str_equal (name, "sub"))  return "<sub>";
  if (g_str_equal (name, "sup"))  return "<sup>";
  if (g_str_equal (name, "s") || g_str_equal (name, "strike") ||
      g_str_equal (name, "del"))  return "<s>";
  if (g_str_equal (name, "u") || g_str_equal (name, "ins") ||
      g_str_equal (name, "a"))    return "<u>";
  return NULL;
}
static const char *
xhtml_inline_close (const char *name)
{
  if (g_str_equal (name, "em") || g_str_equal (name, "i"))    return "</i>";
  if (g_str_equal (name, "strong") || g_str_equal (name, "b"))return "</b>";
  if (g_str_equal (name, "code") || g_str_equal (name, "tt") ||
      g_str_equal (name, "kbd")  || g_str_equal (name, "samp"))return "</tt>";
  if (g_str_equal (name, "sub"))  return "</sub>";
  if (g_str_equal (name, "sup"))  return "</sup>";
  if (g_str_equal (name, "s") || g_str_equal (name, "strike") ||
      g_str_equal (name, "del"))  return "</s>";
  if (g_str_equal (name, "u") || g_str_equal (name, "ins") ||
      g_str_equal (name, "a"))    return "</u>";
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
xhtml_flush_accum (XhtmlCtx *cc)
{
  if (!cc->accum_active) return;
  cc->accum_active = FALSE;
  while (cc->accum->len > 0 &&
         g_ascii_isspace (cc->accum->str[cc->accum->len - 1]))
    g_string_truncate (cc->accum, cc->accum->len - 1);

  if (cc->accum->len > 0) {
    /* Push CHAPTER marker before the very first content block of
     * this spine entry — anchored to the chapter path so NCX
     * `<content src="chapter.html">` lookups land on it. */
    if (!cc->chapter_marker_pushed) {
      FwBlock *chap = fw_block_new (FW_BLOCK_CHAPTER, 0, NULL, NULL,
                                    cc->chapter_path, 0);
      g_list_store_append (cc->blocks, chap);
      if (cc->chapter_path) {
        guint pos = g_list_model_get_n_items (G_LIST_MODEL (cc->blocks)) - 1;
        g_hash_table_insert (cc->anchors,
                             g_strdup (cc->chapter_path),
                             GUINT_TO_POINTER (pos + 1));
      }
      g_object_unref (chap);
      cc->chapter_marker_pushed = TRUE;
    }

    FwBlock *b = fw_block_new (cc->accum_kind, cc->accum_level,
                               cc->accum->str,
                               NULL, cc->pending_anchor, 0);
    g_list_store_append (cc->blocks, b);
    if (cc->pending_anchor) {
      guint pos = g_list_model_get_n_items (G_LIST_MODEL (cc->blocks)) - 1;
      /* Compose a chapter#fragment anchor key — NCX entries pointing
       * to mid-chapter anchors use the `chapter.html#frag` form. */
      g_autofree char *key =
        cc->chapter_path
          ? g_strdup_printf ("%s#%s", cc->chapter_path, cc->pending_anchor)
          : g_strdup (cc->pending_anchor);
      g_hash_table_insert (cc->anchors, g_steal_pointer (&key),
                           GUINT_TO_POINTER (pos + 1));
    }
    g_object_unref (b);
  }
  g_string_truncate (cc->accum, 0);
  g_clear_pointer (&cc->pending_anchor, g_free);
  cc->accum_level = 0;
}

static void
xhtml_start_block (XhtmlCtx     *cc,
                   FwBlockKind   kind,
                   int           level,
                   const char  **attr_names,
                   const char  **attr_values)
{
  xhtml_flush_accum (cc);
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
xhtml_start (GMarkupParseContext *ctx G_GNUC_UNUSED,
             const gchar         *name,
             const gchar        **attr_names,
             const gchar        **attr_values,
             gpointer             user_data,
             GError             **error G_GNUC_UNUSED)
{
  XhtmlCtx *cc = user_data;

  if (g_str_equal (name, "body")) { cc->in_body = TRUE; return; }
  if (!cc->in_body) return;

  int hlvl = heading_level (name);
  if (hlvl > 0) {
    xhtml_start_block (cc, FW_BLOCK_HEADING, hlvl, attr_names, attr_values);
    return;
  }

  if (g_str_equal (name, "p")) {
    xhtml_start_block (cc, FW_BLOCK_PARAGRAPH, 0, attr_names, attr_values);
    return;
  }

  if (g_str_equal (name, "blockquote")) {
    xhtml_start_block (cc, FW_BLOCK_BLOCKQUOTE, 0, attr_names, attr_values);
    return;
  }

  if (g_str_equal (name, "pre")) {
    xhtml_start_block (cc, FW_BLOCK_CODE, 0, attr_names, attr_values);
    return;
  }

  if (g_str_equal (name, "li")) {
    xhtml_start_block (cc, FW_BLOCK_PARAGRAPH, 0, attr_names, attr_values);
    g_string_append (cc->accum, "•  ");
    return;
  }

  if (g_str_equal (name, "hr")) {
    xhtml_flush_accum (cc);
    FwBlock *hr = fw_block_new (FW_BLOCK_HR, 0, NULL, NULL, NULL, 0);
    g_list_store_append (cc->blocks, hr);
    g_object_unref (hr);
    return;
  }

  if (g_str_equal (name, "br")) {
    if (cc->accum_active)
      g_string_append_c (cc->accum, '\n');
    return;
  }

  if (g_str_equal (name, "img")) {
    /* Block image — flush current accumulator and push IMAGE block. */
    const char *src = NULL;
    for (int i = 0; attr_names[i]; i++) {
      if (g_str_equal (attr_names[i], "src")) { src = attr_values[i]; break; }
    }
    if (src) {
      g_autofree char *resolved = NULL;
      if (cc->chapter_path) {
        g_autofree char *chap_dir = dirname_zip (cc->chapter_path);
        resolved = resolve_zip_path (chap_dir, src);
      }
      xhtml_flush_accum (cc);
      FwBlock *img = fw_block_new (FW_BLOCK_IMAGE, 0, NULL,
                                   resolved ? resolved : src, NULL, 0);
      g_list_store_append (cc->blocks, img);
      g_object_unref (img);
    }
    return;
  }

  /* Inline tags inside an active accumulator. */
  if (cc->accum_active) {
    const char *open = xhtml_inline_open (name);
    if (open)
      g_string_append (cc->accum, open);
  }
}

static void
xhtml_end (GMarkupParseContext *ctx G_GNUC_UNUSED,
           const gchar         *name,
           gpointer             user_data,
           GError             **error G_GNUC_UNUSED)
{
  XhtmlCtx *cc = user_data;

  if (g_str_equal (name, "body")) { cc->in_body = FALSE; return; }
  if (!cc->in_body) return;

  if (heading_level (name) > 0 ||
      g_str_equal (name, "p") ||
      g_str_equal (name, "blockquote") ||
      g_str_equal (name, "pre") ||
      g_str_equal (name, "li")) {
    xhtml_flush_accum (cc);
    return;
  }

  if (cc->accum_active) {
    const char *close = xhtml_inline_close (name);
    if (close)
      g_string_append (cc->accum, close);
  }
}

static void
xhtml_text (GMarkupParseContext *ctx G_GNUC_UNUSED,
            const gchar         *text,
            gsize                len,
            gpointer             user_data,
            GError             **error G_GNUC_UNUSED)
{
  XhtmlCtx *cc = user_data;
  if (!cc->accum_active) return;
  /* Collapse runs of whitespace to a single space — XHTML's pretty-
   * printed indent in source would otherwise leak into rendered text. */
  for (gsize i = 0; i < len; i++) {
    char c = text[i];
    if (c == '\n' || c == '\r' || c == '\t')
      c = ' ';
    if (c == ' ' && cc->accum->len > 0 &&
        cc->accum->str[cc->accum->len - 1] == ' ')
      continue;
    if (c == '<')      g_string_append (cc->accum, "&lt;");
    else if (c == '>') g_string_append (cc->accum, "&gt;");
    else if (c == '&') g_string_append (cc->accum, "&amp;");
    else               g_string_append_c (cc->accum, c);
  }
}

static void
parse_xhtml_chapter (FwReflowDocumentEpub *self,
                     const char           *chapter_path,
                     GBytes               *bytes)
{
  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);

  XhtmlCtx cc = {
    .blocks       = self->blocks,
    .anchors      = self->anchors,
    .chapter_path = chapter_path,
    .accum        = g_string_new (NULL),
  };

  static const GMarkupParser p = {
    .start_element = xhtml_start,
    .end_element   = xhtml_end,
    .text          = xhtml_text,
  };

  g_autoptr (GError) e = NULL;
  GMarkupParseContext *gpc =
    g_markup_parse_context_new (&p, G_MARKUP_TREAT_CDATA_AS_TEXT, &cc, NULL);

  gboolean ok =
    g_markup_parse_context_parse (gpc, data, len, &e) &&
    g_markup_parse_context_end_parse (gpc, &e);

  if (ok)
    xhtml_flush_accum (&cc);
  else
    g_warning ("epub: skipping chapter '%s' (parse error: %s)",
               chapter_path, e ? e->message : "(unknown)");

  g_markup_parse_context_free (gpc);
  g_string_free (cc.accum, TRUE);
  g_clear_pointer (&cc.pending_anchor, g_free);
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
        if (frag)
          nc->cur_src = g_strconcat (resolved, frag, NULL);
        else
          nc->cur_src = g_steal_pointer (&resolved);
        break;
      }
    }
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
    /* Flush a TOC entry from the collected title + src. */
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

/* ── Top-level open() ─────────────────────────────────────────────── */

static gboolean
epub_open (FwReflowDocument *doc, const char *path, GError **error)
{
  FwReflowDocumentEpub *self = FW_REFLOW_DOCUMENT_EPUB (doc);

  if (!read_zip_entries (self, path, error))
    return FALSE;

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
      return FALSE;
    }
  }

  /* Walk spine and parse each chapter's XHTML. */
  for (guint i = 0; i < oc.spine->len; i++) {
    const char *idref = oc.spine->pdata[i];
    const char *href  = g_hash_table_lookup (oc.manifest_href, idref);
    if (!href) {
      g_warning ("epub: spine idref '%s' not in manifest", idref);
      continue;
    }
    GBytes *cb = g_hash_table_lookup (self->zip, href);
    if (!cb) {
      g_warning ("epub: chapter '%s' missing from archive", href);
      continue;
    }
    parse_xhtml_chapter (self, href, cb);
  }

  /* TOC: prefer NCX (EPUB 2 / EPUB 3 fallback). */
  if (oc.toc_id) {
    const char *ncx_href = g_hash_table_lookup (oc.manifest_href, oc.toc_id);
    if (ncx_href) {
      GBytes *nb = g_hash_table_lookup (self->zip, ncx_href);
      if (nb)
        parse_ncx (self, ncx_href, nb);
    }
  }

  /* Decode every manifest image into the images hash. Keys: both the
   * resolved zip path (which is what xhtml_start writes into IMAGE
   * blocks) and the manifest id (for hand-rolled callers). */
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
    g_autoptr (GError) e = NULL;
    GdkTexture *tex = gdk_texture_new_from_bytes (b, &e);
    if (tex) {
      g_hash_table_insert (self->images, g_strdup (href), tex);
      /* Second key: manifest id, sharing the same texture. */
      g_hash_table_insert (self->images, g_strdup (id), g_object_ref (tex));
    } else {
      g_warning ("epub: dropping image '%s': %s", href,
                 e ? e->message : "(unknown)");
    }
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

  return TRUE;
}

static void epub_close (FwReflowDocument *doc) {
  FwReflowDocumentEpub *self = FW_REFLOW_DOCUMENT_EPUB (doc);
  if (self->blocks) g_list_store_remove_all (self->blocks);
  if (self->toc)    g_list_store_remove_all (self->toc);
}

static GListModel *epub_get_block_model (FwReflowDocument *doc) {
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_EPUB (doc)->blocks);
}
static GdkTexture *epub_get_image (FwReflowDocument *doc, const char *image_id) {
  if (!image_id) return NULL;
  return g_hash_table_lookup (FW_REFLOW_DOCUMENT_EPUB (doc)->images, image_id);
}
static GListModel *epub_get_toc (FwReflowDocument *doc) {
  return G_LIST_MODEL (FW_REFLOW_DOCUMENT_EPUB (doc)->toc);
}
static guint epub_find_block_by_anchor (FwReflowDocument *doc, const char *a) {
  if (!a) return 0;
  return GPOINTER_TO_UINT (
    g_hash_table_lookup (FW_REFLOW_DOCUMENT_EPUB (doc)->anchors, a));
}
static GArray *epub_search (FwReflowDocument *doc, const char *needle) {
  (void) doc; (void) needle;
  return NULL;
}
static GHashTable *epub_get_metadata (FwReflowDocument *doc) {
  FwReflowDocumentEpub *self = FW_REFLOW_DOCUMENT_EPUB (doc);
  return self->metadata ? g_hash_table_ref (self->metadata) : NULL;
}

static void
fw_reflow_document_epub_iface_init (FwReflowDocumentInterface *iface)
{
  iface->open                  = epub_open;
  iface->close                 = epub_close;
  iface->get_block_model       = epub_get_block_model;
  iface->get_image             = epub_get_image;
  iface->get_toc               = epub_get_toc;
  iface->find_block_by_anchor  = epub_find_block_by_anchor;
  iface->search                = epub_search;
  iface->get_metadata          = epub_get_metadata;
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_reflow_document_epub_finalize (GObject *object)
{
  FwReflowDocumentEpub *self = FW_REFLOW_DOCUMENT_EPUB (object);
  g_clear_object (&self->blocks);
  g_clear_object (&self->toc);
  g_clear_pointer (&self->metadata, g_hash_table_unref);
  g_clear_pointer (&self->images,   g_hash_table_unref);
  g_clear_pointer (&self->anchors,  g_hash_table_unref);
  g_clear_pointer (&self->zip,      g_hash_table_unref);
  g_clear_pointer (&self->path,     g_free);
  g_clear_pointer (&self->opf_dir,  g_free);
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
  self->blocks   = g_list_store_new (FW_TYPE_BLOCK);
  self->toc      = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
  self->metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, g_free);
  self->images   = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, g_object_unref);
  self->anchors  = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, NULL);
  self->zip      = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free,
                                          (GDestroyNotify) g_bytes_unref);
}

FwReflowDocumentEpub *
fw_reflow_document_epub_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_DOCUMENT_EPUB, NULL);
}
