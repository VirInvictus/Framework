/* fw-reflow-document-fb2.c — FB2 (FictionBook 2) reflow backend
 *
 * Walks the file with GMarkupParser. Section nesting maps to heading
 * level + CHAPTER markers; paragraphs become PARAGRAPH blocks with
 * inline <emphasis>/<strong>/<code>/<sub>/<sup>/<strikethrough>
 * translated to Pango markup; <image l:href="#X"/> references resolve
 * against the document's <binary id="X"> images, decoded from base64
 * into GdkTextures. <description>/<title-info> populates the
 * metadata hash.
 *
 * Bare .fb2 only — .fb2.zip is a follow-up. Non-UTF-8 XML declarations
 * are converted via g_convert before parsing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document-fb2.h"

#include <string.h>

struct _FwReflowDocumentFb2 {
  GObject       parent_instance;
  GListStore   *blocks;     /* GListStore<FwBlock> */
  GListStore   *toc;        /* GListStore<FwReflowTocItem> */
  GHashTable   *metadata;   /* gchar* → gchar* */
  GHashTable   *images;     /* gchar* (id) → GdkTexture* (owned) */
  GHashTable   *anchors;    /* gchar* (anchor_id) → guint position */
  char         *path;
};

static void fw_reflow_document_fb2_iface_init (FwReflowDocumentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwReflowDocumentFb2,
                               fw_reflow_document_fb2,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (FW_TYPE_REFLOW_DOCUMENT,
                                                      fw_reflow_document_fb2_iface_init))

/* ── Parser state ─────────────────────────────────────────────────── */

typedef struct {
  FwReflowDocumentFb2 *doc;

  /* Body / section tracking */
  int           section_depth;       /* 0 = outside any section */
  char         *pending_section_id;  /* most recent section's id, awaiting title */

  /* Block accumulator. Active only between matched <p>/<title> opens
   * and the corresponding close. Text events while non-NULL append
   * (Pango-escaped) into `accum`. Inline-style tags add open/close
   * Pango-markup spans. */
  FwBlockKind   accum_kind;
  gboolean      accum_active;
  GString      *accum;
  int           accum_level;     /* heading level if HEADING */
  char         *accum_anchor;    /* anchor id when block flushes */

  /* While inside <title>, we collect a plain-text mirror for TOC. */
  GString      *toc_title;

  /* Description / metadata */
  gboolean      in_description;
  gboolean      in_title_info;
  gboolean      in_author;
  GString      *meta_text;       /* current scalar meta accumulator */
  const char   *meta_key;        /* canonical key for meta_text */
  GString      *author_first;
  GString      *author_middle;
  GString      *author_last;

  /* Binary collection */
  GString      *bin_data;
  char         *bin_id;
  char         *bin_type;
} Fb2Ctx;

/* ── Helpers ──────────────────────────────────────────────────────── */

static const char *
attr_get (const char **names, const char **values, const char *want)
{
  for (int i = 0; names[i]; i++)
    if (g_ascii_strcasecmp (names[i], want) == 0)
      return values[i];
  return NULL;
}

/* href can arrive as l:href, xlink:href, or plain href depending on
 * the FB2 producer's namespace declarations. */
static const char *
attr_get_href (const char **names, const char **values)
{
  static const char *candidates[] = { "l:href", "xlink:href", "href", NULL };
  for (int c = 0; candidates[c]; c++) {
    const char *v = attr_get (names, values, candidates[c]);
    if (v)
      return v;
  }
  return NULL;
}

static void
flush_accum_to_block (Fb2Ctx *ctx)
{
  if (!ctx->accum_active)
    return;

  ctx->accum_active = FALSE;
  if (ctx->accum && ctx->accum->len > 0) {
    /* Trim trailing whitespace — FB2 sources frequently put a newline
     * before the closing tag of a paragraph, which would otherwise
     * make every paragraph render with a blank line at the end. */
    while (ctx->accum->len > 0 &&
           g_ascii_isspace (ctx->accum->str[ctx->accum->len - 1]))
      g_string_truncate (ctx->accum, ctx->accum->len - 1);

    if (ctx->accum->len > 0) {
      FwBlock *b = fw_block_new (ctx->accum_kind, ctx->accum_level,
                                 ctx->accum->str,
                                 NULL, ctx->accum_anchor, 0);
      g_list_store_append (ctx->doc->blocks, b);

      if (ctx->accum_anchor) {
        guint pos = g_list_model_get_n_items (G_LIST_MODEL (ctx->doc->blocks)) - 1;
        g_hash_table_insert (ctx->doc->anchors,
                             g_strdup (ctx->accum_anchor),
                             GUINT_TO_POINTER (pos + 1));   /* +1 so 0 means "not found" */
      }
      g_object_unref (b);
    }
  }
  if (ctx->accum)
    g_string_truncate (ctx->accum, 0);
  g_clear_pointer (&ctx->accum_anchor, g_free);
  ctx->accum_level = 0;
}

/* Map FB2 inline element name to a Pango span open tag. Returns NULL
 * for unrecognized inlines (caller leaves the tag in place but text
 * still flows). */
static const char *
inline_open (const char *name)
{
  if (g_str_equal (name, "emphasis"))     return "<i>";
  if (g_str_equal (name, "strong"))       return "<b>";
  if (g_str_equal (name, "code"))         return "<tt>";
  if (g_str_equal (name, "sub"))          return "<sub>";
  if (g_str_equal (name, "sup"))          return "<sup>";
  if (g_str_equal (name, "strikethrough"))return "<s>";
  return NULL;
}
static const char *
inline_close (const char *name)
{
  if (g_str_equal (name, "emphasis"))     return "</i>";
  if (g_str_equal (name, "strong"))       return "</b>";
  if (g_str_equal (name, "code"))         return "</tt>";
  if (g_str_equal (name, "sub"))          return "</sub>";
  if (g_str_equal (name, "sup"))          return "</sup>";
  if (g_str_equal (name, "strikethrough"))return "</s>";
  return NULL;
}

/* ── start/end element callbacks ──────────────────────────────────── */

static void
on_start (GMarkupParseContext  *context G_GNUC_UNUSED,
          const gchar          *name,
          const gchar         **attr_names,
          const gchar         **attr_values,
          gpointer              user_data,
          GError              **error G_GNUC_UNUSED)
{
  Fb2Ctx *ctx = user_data;

  /* description / title-info / metadata scopes */
  if (g_str_equal (name, "description"))   { ctx->in_description = TRUE; return; }
  if (g_str_equal (name, "title-info")) {
    if (ctx->in_description) ctx->in_title_info = TRUE;
    return;
  }
  if (ctx->in_title_info) {
    if (g_str_equal (name, "author")) { ctx->in_author = TRUE; return; }
    if (ctx->in_author) {
      if (g_str_equal (name, "first-name") || g_str_equal (name, "middle-name") ||
          g_str_equal (name, "last-name")) {
        if (!ctx->meta_text) ctx->meta_text = g_string_new (NULL);
        g_string_truncate (ctx->meta_text, 0);
        ctx->meta_key = name;
        return;
      }
    }
    if (g_str_equal (name, "book-title") || g_str_equal (name, "lang") ||
        g_str_equal (name, "src-lang")   || g_str_equal (name, "annotation")) {
      if (!ctx->meta_text) ctx->meta_text = g_string_new (NULL);
      g_string_truncate (ctx->meta_text, 0);
      ctx->meta_key = name;
      return;
    }
  }

  /* binary */
  if (g_str_equal (name, "binary")) {
    g_clear_pointer (&ctx->bin_id,   g_free);
    g_clear_pointer (&ctx->bin_type, g_free);
    if (ctx->bin_data) g_string_truncate (ctx->bin_data, 0);
    else               ctx->bin_data = g_string_new (NULL);
    const char *id = attr_get (attr_names, attr_values, "id");
    const char *ct = attr_get (attr_names, attr_values, "content-type");
    if (id) ctx->bin_id   = g_strdup (id);
    if (ct) ctx->bin_type = g_strdup (ct);
    return;
  }

  /* section — emit a CHAPTER marker, hold the id for the upcoming title */
  if (g_str_equal (name, "section")) {
    ctx->section_depth++;
    g_clear_pointer (&ctx->pending_section_id, g_free);
    const char *id = attr_get (attr_names, attr_values, "id");
    if (id)
      ctx->pending_section_id = g_strdup (id);

    /* Push CHAPTER marker — view paints nothing for it; we keep it
     * because future scroll-to-anchor logic needs a separator block. */
    FwBlock *chap = fw_block_new (FW_BLOCK_CHAPTER,
                                  ctx->section_depth, NULL, NULL,
                                  ctx->pending_section_id, 0);
    g_list_store_append (ctx->doc->blocks, chap);
    g_object_unref (chap);
    return;
  }

  /* image — push IMAGE block (inline images become block-level for
   * now; the design doc explicitly accepts this for v1). */
  if (g_str_equal (name, "image")) {
    flush_accum_to_block (ctx);
    const char *href = attr_get_href (attr_names, attr_values);
    if (href) {
      const char *id = href[0] == '#' ? href + 1 : href;
      FwBlock *img = fw_block_new (FW_BLOCK_IMAGE, 0, NULL, id, NULL, 0);
      g_list_store_append (ctx->doc->blocks, img);
      g_object_unref (img);
    }
    return;
  }

  if (g_str_equal (name, "empty-line")) {
    flush_accum_to_block (ctx);
    FwBlock *hr = fw_block_new (FW_BLOCK_HR, 0, NULL, NULL, NULL, 0);
    g_list_store_append (ctx->doc->blocks, hr);
    g_object_unref (hr);
    return;
  }

  /* title — start HEADING accumulator (only meaningful inside a section).
   * Children (<p>) won't open new accums while we're already accumulating
   * a HEADING. */
  if (g_str_equal (name, "title") && ctx->section_depth > 0) {
    flush_accum_to_block (ctx);
    if (!ctx->accum) ctx->accum = g_string_new (NULL);
    g_string_truncate (ctx->accum, 0);
    ctx->accum_active = TRUE;
    ctx->accum_kind   = FW_BLOCK_HEADING;
    ctx->accum_level  = ctx->section_depth;
    g_clear_pointer (&ctx->accum_anchor, g_free);
    if (ctx->pending_section_id) {
      ctx->accum_anchor = ctx->pending_section_id;  /* transfer */
      ctx->pending_section_id = NULL;
    }
    if (!ctx->toc_title) ctx->toc_title = g_string_new (NULL);
    g_string_truncate (ctx->toc_title, 0);
    return;
  }

  /* paragraph (or anything inside an active HEADING accumulator —
   * <p> inside <title> contributes to the heading's text rather than
   * starting a new block). */
  if (g_str_equal (name, "p") || g_str_equal (name, "subtitle")) {
    if (ctx->accum_active && ctx->accum_kind == FW_BLOCK_HEADING) {
      if (ctx->accum->len > 0)
        g_string_append_c (ctx->accum, ' ');
      return;
    }
    flush_accum_to_block (ctx);
    if (!ctx->accum) ctx->accum = g_string_new (NULL);
    g_string_truncate (ctx->accum, 0);
    ctx->accum_active = TRUE;
    ctx->accum_kind   = FW_BLOCK_PARAGRAPH;
    ctx->accum_level  = 0;
    g_clear_pointer (&ctx->accum_anchor, g_free);
    return;
  }

  /* cite / epigraph — blockquote */
  if (g_str_equal (name, "cite") || g_str_equal (name, "epigraph")) {
    flush_accum_to_block (ctx);
    if (!ctx->accum) ctx->accum = g_string_new (NULL);
    g_string_truncate (ctx->accum, 0);
    ctx->accum_active = TRUE;
    ctx->accum_kind   = FW_BLOCK_BLOCKQUOTE;
    ctx->accum_level  = 0;
    return;
  }

  /* Inline styles inside an active accumulator — append Pango open. */
  if (ctx->accum_active) {
    const char *open = inline_open (name);
    if (open)
      g_string_append (ctx->accum, open);
    return;
  }
}

static void
on_end (GMarkupParseContext  *context G_GNUC_UNUSED,
        const gchar          *name,
        gpointer              user_data,
        GError              **error G_GNUC_UNUSED)
{
  Fb2Ctx *ctx = user_data;

  if (g_str_equal (name, "description"))   { ctx->in_description = FALSE; return; }
  if (g_str_equal (name, "title-info")) {
    ctx->in_title_info = FALSE;
    /* On title-info close, build a single "author" entry from the
     * collected name pieces. */
    GString *au = g_string_new (NULL);
    if (ctx->author_first  && ctx->author_first->len)  g_string_append (au, ctx->author_first->str);
    if (ctx->author_middle && ctx->author_middle->len) {
      if (au->len) g_string_append_c (au, ' ');
      g_string_append (au, ctx->author_middle->str);
    }
    if (ctx->author_last   && ctx->author_last->len) {
      if (au->len) g_string_append_c (au, ' ');
      g_string_append (au, ctx->author_last->str);
    }
    if (au->len > 0)
      g_hash_table_insert (ctx->doc->metadata,
                           g_strdup ("author"), g_string_free (au, FALSE));
    else
      g_string_free (au, TRUE);
    return;
  }

  if (g_str_equal (name, "author")) { ctx->in_author = FALSE; return; }

  /* Metadata scalar close. */
  if (ctx->meta_text && ctx->meta_key && g_str_equal (name, ctx->meta_key)) {
    /* Trim whitespace */
    while (ctx->meta_text->len > 0 &&
           g_ascii_isspace (ctx->meta_text->str[ctx->meta_text->len - 1]))
      g_string_truncate (ctx->meta_text, ctx->meta_text->len - 1);

    if (ctx->in_author) {
      if (g_str_equal (ctx->meta_key, "first-name")) {
        if (!ctx->author_first)  ctx->author_first  = g_string_new (NULL);
        g_string_assign (ctx->author_first, ctx->meta_text->str);
      } else if (g_str_equal (ctx->meta_key, "middle-name")) {
        if (!ctx->author_middle) ctx->author_middle = g_string_new (NULL);
        g_string_assign (ctx->author_middle, ctx->meta_text->str);
      } else if (g_str_equal (ctx->meta_key, "last-name")) {
        if (!ctx->author_last)   ctx->author_last   = g_string_new (NULL);
        g_string_assign (ctx->author_last, ctx->meta_text->str);
      }
    } else {
      const char *canonical = ctx->meta_key;
      if (g_str_equal (canonical, "book-title")) canonical = "title";
      g_hash_table_insert (ctx->doc->metadata,
                           g_strdup (canonical),
                           g_strdup (ctx->meta_text->str));
    }
    g_string_truncate (ctx->meta_text, 0);
    ctx->meta_key = NULL;
    return;
  }

  /* binary: decode base64 → GBytes → GdkTexture */
  if (g_str_equal (name, "binary")) {
    if (ctx->bin_id && ctx->bin_data && ctx->bin_data->len > 0) {
      gsize out_len = 0;
      g_autofree guchar *bytes = g_base64_decode (ctx->bin_data->str, &out_len);
      if (bytes && out_len > 0) {
        g_autoptr (GBytes) gb = g_bytes_new (bytes, out_len);
        g_autoptr (GError) e = NULL;
        GdkTexture *tex = gdk_texture_new_from_bytes (gb, &e);
        if (tex)
          g_hash_table_insert (ctx->doc->images, g_strdup (ctx->bin_id), tex);
        else
          g_warning ("fb2: dropping image '%s': %s",
                     ctx->bin_id, e ? e->message : "(unknown)");
      }
    }
    g_clear_pointer (&ctx->bin_id,   g_free);
    g_clear_pointer (&ctx->bin_type, g_free);
    if (ctx->bin_data) g_string_truncate (ctx->bin_data, 0);
    return;
  }

  if (g_str_equal (name, "section")) {
    flush_accum_to_block (ctx);
    if (ctx->section_depth > 0)
      ctx->section_depth--;
    g_clear_pointer (&ctx->pending_section_id, g_free);
    return;
  }

  if (g_str_equal (name, "title")) {
    /* Push TOC entry from the plain text we mirrored, then flush
     * the heading accumulator. */
    if (ctx->toc_title && ctx->toc_title->len > 0) {
      const char *anchor = ctx->accum_anchor;
      FwReflowTocItem *item =
        fw_reflow_toc_item_new (ctx->toc_title->str, anchor);
      g_list_store_append (ctx->doc->toc, item);
      g_object_unref (item);
    }
    if (ctx->toc_title) g_string_truncate (ctx->toc_title, 0);
    flush_accum_to_block (ctx);
    return;
  }

  if (g_str_equal (name, "p") || g_str_equal (name, "subtitle")) {
    if (ctx->accum_active && ctx->accum_kind == FW_BLOCK_HEADING)
      return;  /* part of a heading; don't flush */
    flush_accum_to_block (ctx);
    return;
  }

  if (g_str_equal (name, "cite") || g_str_equal (name, "epigraph")) {
    flush_accum_to_block (ctx);
    return;
  }

  if (ctx->accum_active) {
    const char *close = inline_close (name);
    if (close)
      g_string_append (ctx->accum, close);
  }
}

static void
on_text (GMarkupParseContext *context G_GNUC_UNUSED,
         const gchar         *text,
         gsize                text_len,
         gpointer             user_data,
         GError             **error G_GNUC_UNUSED)
{
  Fb2Ctx *ctx = user_data;

  /* Binary data — accumulate raw (we'll base64-decode at close). */
  if (ctx->bin_id && ctx->bin_data) {
    g_string_append_len (ctx->bin_data, text, text_len);
    return;
  }

  /* Metadata scalar */
  if (ctx->meta_text && ctx->meta_key) {
    g_string_append_len (ctx->meta_text, text, text_len);
    return;
  }

  /* Block accumulator */
  if (ctx->accum_active) {
    g_autofree char *escaped = g_markup_escape_text (text, text_len);
    g_string_append (ctx->accum, escaped);

    if (ctx->accum_kind == FW_BLOCK_HEADING && ctx->toc_title)
      g_string_append_len (ctx->toc_title, text, text_len);
  }
}

/* ── Encoding normalization (XML prolog) ──────────────────────────── */

/* If the file declares a non-UTF-8 encoding, convert it to UTF-8 in
 * place. GMarkupParser only understands UTF-8. Returns a fresh UTF-8
 * buffer (caller frees) and writes its length to *out_len. On
 * conversion failure or unknown encoding, returns NULL with error set. */
static char *
normalize_to_utf8 (const char *raw, gsize len, gsize *out_len, GError **error)
{
  /* Look for encoding="..." inside the first ~256 bytes. */
  gsize scan = MIN (len, 256u);
  const char *encbeg = g_strstr_len (raw, scan, "encoding=\"");
  const char *encname = NULL;
  gsize       enclen  = 0;
  if (encbeg) {
    encbeg += strlen ("encoding=\"");
    const char *encend = memchr (encbeg, '"', scan - (encbeg - raw));
    if (encend) {
      encname = encbeg;
      enclen  = encend - encbeg;
    }
  }

  if (!encname || enclen == 0 ||
      g_ascii_strncasecmp (encname, "UTF-8", MAX (enclen, 5u)) == 0 ||
      g_ascii_strncasecmp (encname, "utf8",  MAX (enclen, 4u)) == 0) {
    /* Already UTF-8 (or no declaration — assume UTF-8). */
    *out_len = len;
    return g_memdup2 (raw, len);
  }

  g_autofree char *encname_z = g_strndup (encname, enclen);
  char *converted = g_convert (raw, len, "UTF-8", encname_z,
                               NULL, out_len, error);
  return converted;
}

/* ── Interface implementation ─────────────────────────────────────── */

static void
fb2_ctx_clear (Fb2Ctx *ctx)
{
  g_clear_pointer (&ctx->pending_section_id, g_free);
  g_clear_pointer (&ctx->accum_anchor, g_free);
  g_clear_pointer (&ctx->bin_id, g_free);
  g_clear_pointer (&ctx->bin_type, g_free);
  if (ctx->accum)         g_string_free (ctx->accum, TRUE);
  if (ctx->toc_title)     g_string_free (ctx->toc_title, TRUE);
  if (ctx->meta_text)     g_string_free (ctx->meta_text, TRUE);
  if (ctx->author_first)  g_string_free (ctx->author_first, TRUE);
  if (ctx->author_middle) g_string_free (ctx->author_middle, TRUE);
  if (ctx->author_last)   g_string_free (ctx->author_last, TRUE);
  if (ctx->bin_data)      g_string_free (ctx->bin_data, TRUE);
}

static gboolean
fb2_open (FwReflowDocument *doc, const char *path, GError **error)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);

  g_autofree char *raw = NULL;
  gsize raw_len = 0;
  if (!g_file_get_contents (path, &raw, &raw_len, error))
    return FALSE;

  gsize utf8_len = 0;
  g_autofree char *utf8 = normalize_to_utf8 (raw, raw_len, &utf8_len, error);
  if (!utf8)
    return FALSE;

  static const GMarkupParser parser = {
    .start_element = on_start,
    .end_element   = on_end,
    .text          = on_text,
    .passthrough   = NULL,
    .error         = NULL,
  };

  Fb2Ctx ctx = { .doc = self };

  GMarkupParseContext *pctx =
    g_markup_parse_context_new (&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, &ctx, NULL);

  gboolean ok =
    g_markup_parse_context_parse (pctx, utf8, utf8_len, error) &&
    g_markup_parse_context_end_parse (pctx, error);

  /* Final flush in case the document closed without explicit </p>
   * (defensive — shouldn't happen on well-formed FB2 but the parser
   * accepts text-after-close in non-strict mode). */
  if (ok)
    flush_accum_to_block (&ctx);

  g_markup_parse_context_free (pctx);
  fb2_ctx_clear (&ctx);

  if (!ok)
    return FALSE;

  self->path = g_strdup (path);

  /* Round out metadata. */
  if (!g_hash_table_contains (self->metadata, "title")) {
    g_autofree char *basename = g_path_get_basename (path);
    g_hash_table_insert (self->metadata, g_strdup ("title"), g_strdup (basename));
  }
  g_hash_table_insert (self->metadata, g_strdup ("format"),
                       g_strdup ("FictionBook 2"));

  return TRUE;
}

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
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  return G_LIST_MODEL (self->blocks);
}

static GdkTexture *
fb2_get_image (FwReflowDocument *doc, const char *image_id)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  if (!image_id) return NULL;
  /* href values often arrive with a leading '#' in the block; both
   * keys are accepted. */
  if (image_id[0] == '#')
    image_id++;
  return g_hash_table_lookup (self->images, image_id);
}

static GListModel *
fb2_get_toc (FwReflowDocument *doc)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  return G_LIST_MODEL (self->toc);
}

static guint
fb2_find_block_by_anchor (FwReflowDocument *doc, const char *anchor_id)
{
  FwReflowDocumentFb2 *self = FW_REFLOW_DOCUMENT_FB2 (doc);
  if (!anchor_id) return 0;
  gpointer v = g_hash_table_lookup (self->anchors, anchor_id);
  return GPOINTER_TO_UINT (v);  /* 0 = not found; 1+ = block_pos + 1 */
}

static GArray *
fb2_search (FwReflowDocument *doc, const char *needle)
{
  (void) doc; (void) needle;
  return NULL;  /* Phase 6 polish — search highlight in the listview. */
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
  iface->search                = fb2_search;
  iface->get_metadata          = fb2_get_metadata;
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

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
