/* fw-reflow-view.c — GtkListView host for FwReflowDocument
 *
 * Single-column GtkListView whose factory maps each FwBlock to a
 * native widget (Pango-aware GtkLabel for now). Continuous vertical
 * scroll is GTK's default. The Fractal pattern this is named after is
 * "GListModel of structurally-typed items, factory renders each into
 * a native widget that does its own text wrapping" — same shape, in C.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-view.h"
#include "fw-config.h"
#include <gio/gio.h>

#define READING_COLUMN_MAX_WIDTH 720   /* px — caps comfortable line length */
#define READING_COLUMN_MARGIN     24

/* ── Pagination ────────────────────────────────────────────────────
 * Each page is a contiguous range of whole blocks that fit within
 * viewport height. Blocks are never split mid-paragraph (block-level
 * pagination only). On viewport resize the page table is rebuilt by
 * walking every block and Pango-measuring its height at the current
 * column width; the current_page index is preserved by content
 * (we remember which block the page started on, then look up its
 * new page index after the rebuild).
 */
typedef struct {
  guint first;   /* first block index */
  guint count;   /* number of blocks on this page */
} FwPageRange;

struct _FwReflowView {
  GtkWidget           parent_instance;

  FwReflowDocument   *document;     /* owned */

  GtkScrolledWindow  *scroll;       /* fills our area */
  GtkListView        *list;
  GtkSingleSelection *selection;
  GtkSliceListModel  *page_slice;   /* windows the doc's block model
                                     * to the current page */

  GtkCssProvider     *css;
  GSettings          *settings;
  gulong              settings_handler;

  /* Pagination state */
  GListModel         *all_blocks;   /* unowned (held by document) */
  GArray             *pages;        /* FwPageRange[] */
  guint               current_page;

  /* Repagination triggers */
  int                 last_alloc_w;
  int                 last_alloc_h;
  guint               repaginate_idle;
};

enum {
  SIG_PAGE_CHANGED,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (FwReflowView, fw_reflow_view, GTK_TYPE_WIDGET)

/* ── Factory: FwBlock → GtkWidget ─────────────────────────────────── */

/* Each row hosts a GtkStack with two named children:
 *   "text"  → a wrapping, selectable GtkLabel for paragraph / heading /
 *             code / blockquote / hr / chapter blocks.
 *   "image" → a GtkPicture sized to its natural dimensions, fit-contained
 *             into the row width.
 * Bind switches the visible page; the unused widget is preserved so
 * recycled rows skip widget churn when the kind toggles back.
 *
 * IMPORTANT: keep widget-side margins to zero. All vertical rhythm
 * comes from CSS margin-top/bottom on the per-kind classes, applied
 * conditionally — that way an empty CHAPTER block doesn't double-pad
 * its row, and PARAGRAPH spacing is determined purely by the active
 * line-height setting. The previous 4px widget margins stacked with
 * GtkListView's default row padding to produce ~16px of dead space
 * between every paragraph.
 */

static GtkWidget *
make_text_label (void)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (label, TRUE);
  gtk_label_set_wrap_mode (label, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (label, 0.0);
  gtk_label_set_yalign (label, 0.0);
  gtk_label_set_selectable (label, TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (label), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (label), GTK_ALIGN_FILL);
  gtk_widget_set_margin_start  (GTK_WIDGET (label), READING_COLUMN_MARGIN);
  gtk_widget_set_margin_end    (GTK_WIDGET (label), READING_COLUMN_MARGIN);
  gtk_widget_add_css_class     (GTK_WIDGET (label), "reflow-paragraph");
  return GTK_WIDGET (label);
}

static GtkWidget *
make_image_picture (void)
{
  GtkPicture *pic = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_can_shrink     (pic, TRUE);
  gtk_picture_set_content_fit    (pic, GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_halign          (GTK_WIDGET (pic), GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand         (GTK_WIDGET (pic), TRUE);
  gtk_widget_set_margin_start    (GTK_WIDGET (pic), READING_COLUMN_MARGIN);
  gtk_widget_set_margin_end      (GTK_WIDGET (pic), READING_COLUMN_MARGIN);
  gtk_widget_add_css_class       (GTK_WIDGET (pic), "reflow-image");
  return GTK_WIDGET (pic);
}

static void
on_factory_setup (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                  GObject                  *listitem,
                  gpointer                  user_data G_GNUC_UNUSED)
{
  GtkStack *stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (stack, GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_stack_add_named (stack, make_text_label (),    "text");
  gtk_stack_add_named (stack, make_image_picture (), "image");
  gtk_list_item_set_child (GTK_LIST_ITEM (listitem), GTK_WIDGET (stack));
}

static void
on_factory_bind (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                 GObject                  *listitem,
                 gpointer                  user_data)
{
  FwReflowView *self  = FW_REFLOW_VIEW (user_data);
  GtkListItem  *item  = GTK_LIST_ITEM (listitem);
  GtkStack     *stack = GTK_STACK (gtk_list_item_get_child (item));
  FwBlock      *block = FW_BLOCK (gtk_list_item_get_item (item));

  if (!stack || !block)
    return;

  GtkWidget *text_w  = gtk_stack_get_child_by_name (stack, "text");
  GtkWidget *image_w = gtk_stack_get_child_by_name (stack, "image");

  /* Image branch — try to resolve the texture; fall through to the
   * text page if the image isn't available. */
  if (fw_block_get_kind (block) == FW_BLOCK_IMAGE) {
    const char *id  = fw_block_get_image_id (block);
    GdkTexture *tex = NULL;
    if (id && self->document)
      tex = fw_reflow_document_get_image (self->document, id);
    if (tex) {
      gtk_picture_set_paintable (GTK_PICTURE (image_w), GDK_PAINTABLE (tex));
      gtk_stack_set_visible_child_name (stack, "image");
      return;
    }
    /* fall through to text fallback */
    GtkLabel *label = GTK_LABEL (text_w);
    gtk_label_set_use_markup (label, FALSE);
    gtk_label_set_text (label, id ? id : "[image]");
    gtk_stack_set_visible_child_name (stack, "text");
    return;
  }

  /* Text branch */
  GtkLabel *label = GTK_LABEL (text_w);
  gtk_widget_remove_css_class (text_w, "reflow-heading");
  gtk_widget_remove_css_class (text_w, "reflow-code");
  gtk_widget_remove_css_class (text_w, "reflow-blockquote");
  gtk_widget_remove_css_class (text_w, "reflow-chapter");

  switch (fw_block_get_kind (block)) {
    case FW_BLOCK_HEADING: {
      gtk_widget_add_css_class (text_w, "reflow-heading");
      g_autofree char *cls = g_strdup_printf ("reflow-h%d",
                                              CLAMP (fw_block_get_level (block), 1, 6));
      gtk_widget_add_css_class (text_w, cls);
      gtk_label_set_use_markup (label, TRUE);
      gtk_label_set_markup (label, fw_block_get_text (block) ?: "");
      break;
    }
    case FW_BLOCK_CODE:
      gtk_widget_add_css_class (text_w, "reflow-code");
      gtk_label_set_use_markup (label, FALSE);
      gtk_label_set_text (label, fw_block_get_text (block) ?: "");
      break;
    case FW_BLOCK_BLOCKQUOTE:
      gtk_widget_add_css_class (text_w, "reflow-blockquote");
      gtk_label_set_use_markup (label, TRUE);
      gtk_label_set_markup (label, fw_block_get_text (block) ?: "");
      break;
    case FW_BLOCK_HR:
      gtk_label_set_use_markup (label, FALSE);
      gtk_label_set_text (label, "———");
      break;
    case FW_BLOCK_CHAPTER:
      gtk_widget_add_css_class (text_w, "reflow-chapter");
      gtk_label_set_use_markup (label, FALSE);
      gtk_label_set_text (label, fw_block_get_text (block) ?: "");
      break;
    case FW_BLOCK_LIST:
    case FW_BLOCK_LIST_ITEM:
    case FW_BLOCK_PARAGRAPH:
    case FW_BLOCK_IMAGE:        /* unreachable — handled above */
    default:
      gtk_label_set_use_markup (label, TRUE);
      gtk_label_set_markup (label, fw_block_get_text (block) ?: "");
      break;
  }

  gtk_stack_set_visible_child_name (stack, "text");
}

/* ── CSS for typography (regenerated from GSettings) ──────────────── */

/* Static structural CSS — listview row reset (the source of the
 * "huge whitespace" complaint: GtkListView's default row CSS adds
 * 6–8 px top/bottom padding intended for picker UIs, which stacks
 * onto every paragraph), plus image background and blockquote bar.
 * Stable across font-preference changes so the regeneration only
 * has to rewrite the size/family rules. */
static const char REFLOW_STATIC_CSS[] =
  /* Reset row chrome — no padding, no min-height, no hover or
   * selection highlight. The rows are reading-text containers, not
   * list-picker rows. */
  ".reflow-listview,"
  ".reflow-listview > row,"
  ".reflow-listview > row:hover,"
  ".reflow-listview > row:selected,"
  ".reflow-listview > row:selected:focus,"
  ".reflow-listview > row:focus {"
  "    padding: 0;"
  "    margin: 0;"
  "    min-height: 0;"
  "    background: transparent;"
  "    box-shadow: none;"
  "    outline: none;"
  "    border: none;"
  "}"
  /* Blockquote vertical bar + small left padding. */
  ".reflow-blockquote {"
  "    font-style: italic;"
  "    opacity: 0.85;"
  "    border-left: 3px solid alpha(currentColor, 0.3);"
  "    padding-left: 12px;"
  "    margin-top: 6px;"
  "    margin-bottom: 6px;"
  "}"
  /* CHAPTER blocks have no text — they're a tiny gap that signals a
   * new spine entry / FB2 section. 12 px total (no widget margins,
   * just CSS). */
  ".reflow-chapter {"
  "    min-height: 12px;"
  "    margin: 0;"
  "    padding: 0;"
  "}"
  /* IMAGE rows: a hint of frame around the picture. The picture
   * itself sizes to its natural dimensions, capped by row width via
   * content-fit=contain. */
  ".reflow-image {"
  "    background: alpha(currentColor, 0.03);"
  "    border-radius: 6px;"
  "    margin-top: 8px;"
  "    margin-bottom: 8px;"
  "}";

static char *
build_reflow_css (FwReflowView *self)
{
  /* Read GSettings; fall back to safe defaults if settings aren't
   * bound yet (very early in init). */
  g_autofree char *body_family = NULL;
  g_autofree char *mono_family = NULL;
  double size = 13.0, line_height = 1.5;
  if (self->settings) {
    body_family = g_settings_get_string (self->settings, "reading-font-family");
    mono_family = g_settings_get_string (self->settings, "reading-monospace-family");
    size        = g_settings_get_double (self->settings, "reading-font-size");
    line_height = g_settings_get_double (self->settings, "reading-line-height");
  } else {
    body_family = g_strdup ("");
    mono_family = g_strdup ("");
  }

  /* Default body family: Atkinson Hyperlegible (bundled, designed for
   * high readability) when the user hasn't picked one. The bundled
   * fonts are registered with FontConfig at app startup so this
   * always resolves locally. */
  const char *body_used = (body_family && *body_family)
                            ? body_family
                            : "Atkinson Hyperlegible";

  /* Default monospace: system mono — Source Code Pro is on most
   * dev systems and any GTK install on GNOME ships a fallback. */
  const char *mono_used = (mono_family && *mono_family)
                            ? mono_family
                            : "monospace";

  /* Heading sizes scale relative to the body size. */
  double h1 = size + 9;
  double h2 = size + 5;
  double h3 = size + 2;
  double h4 = size + 1;

  GString *css = g_string_new (REFLOW_STATIC_CSS);

  /* Body family applied to text-bearing classes. The reflow-image
   * rule doesn't need a font; reflow-code overrides with monospace. */
  g_string_append_printf (
    css,
    ".reflow-paragraph, .reflow-heading, .reflow-blockquote, .reflow-chapter "
    "{ font-family: \"%s\"; } "
    ".reflow-code { font-family: \"%s\"; } ",
    body_used, mono_used);

  /* Tight, reading-app-grade vertical rhythm.
   *
   * - Paragraphs: line-height drives the in-block leading; a 0.4em
   *   inter-block margin gives a paragraph break without doubling
   *   into the row gutter (which is now 0). Pango's logical descent
   *   already includes some space below the last line of each
   *   paragraph, so 0.4em is enough to feel like a paragraph break
   *   without floating away from the previous one.
   * - Headings: more breathing room before than after — typical
   *   chapter-heading rhythm. */
  g_string_append_printf (
    css,
    ".reflow-paragraph {"
    "  font-size: %.1fpt;"
    "  line-height: %.2f;"
    "  margin-top: 0;"
    "  margin-bottom: 0.4em;"
    "}"
    ".reflow-heading {"
    "  font-weight: bold;"
    "  line-height: 1.15;"
    "  margin-top: 1.0em;"
    "  margin-bottom: 0.4em;"
    "}"
    ".reflow-h1 { font-size: %.1fpt; }"
    ".reflow-h2 { font-size: %.1fpt; }"
    ".reflow-h3 { font-size: %.1fpt; }"
    ".reflow-h4 { font-size: %.1fpt; }"
    ".reflow-h5 { font-size: %.1fpt; }"
    ".reflow-h6 { font-size: %.1fpt; }"
    ".reflow-code {"
    "  font-size: %.1fpt;"
    "  line-height: %.2f;"
    "}"
    ".reflow-blockquote {"
    "  font-size: %.1fpt;"
    "  line-height: %.2f;"
    "}",
    size, line_height,
    h1, h2, h3, h4, size, size,
    size - 1, line_height,
    size, line_height);

  return g_string_free (css, FALSE);
}

static void
reload_css (FwReflowView *self)
{
  if (!self->css)
    return;
  g_autofree char *css = build_reflow_css (self);
  gtk_css_provider_load_from_string (self->css, css);
}

static void
on_reading_setting_changed (GSettings   *settings G_GNUC_UNUSED,
                            const char  *key      G_GNUC_UNUSED,
                            gpointer     user_data)
{
  reload_css (FW_REFLOW_VIEW (user_data));
}

static void
ensure_css (FwReflowView *self)
{
  if (self->css)
    return;
  self->css = gtk_css_provider_new ();

  if (!self->settings)
    self->settings = g_settings_new (APP_ID);

  reload_css (self);

  gtk_style_context_add_provider_for_display (
    gdk_display_get_default (),
    GTK_STYLE_PROVIDER (self->css),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  /* Single subscriber for all four reading-* keys — the handler
   * regenerates the whole CSS so stale rules don't leak. */
  self->settings_handler = g_signal_connect (
    self->settings, "changed",
    G_CALLBACK (on_reading_setting_changed), self);
}

/* ── Pagination internals ─────────────────────────────────────────── */

/* Build a measurement-only widget mirroring what the factory produces
 * for a given block. The widget is not parented (caller sinks it after
 * measure). CSS classes are set so the display-wide CSS provider's
 * font/size/line-height rules apply during gtk_widget_measure. */
static GtkWidget *
make_measure_widget_for (FwReflowView *self, FwBlock *block)
{
  if (fw_block_get_kind (block) == FW_BLOCK_IMAGE) {
    GtkPicture *pic = GTK_PICTURE (gtk_picture_new ());
    gtk_picture_set_can_shrink  (pic, TRUE);
    gtk_picture_set_content_fit (pic, GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_halign       (GTK_WIDGET (pic), GTK_ALIGN_CENTER);
    gtk_widget_add_css_class    (GTK_WIDGET (pic), "reflow-image");

    const char *id = fw_block_get_image_id (block);
    GdkTexture *tex = (id && self->document)
                       ? fw_reflow_document_get_image (self->document, id)
                       : NULL;
    if (tex)
      gtk_picture_set_paintable (pic, GDK_PAINTABLE (tex));
    return GTK_WIDGET (pic);
  }

  GtkLabel *label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap     (label, TRUE);
  gtk_label_set_wrap_mode (label, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign   (label, 0.0);
  gtk_label_set_yalign   (label, 0.0);
  gtk_widget_set_halign  (GTK_WIDGET (label), GTK_ALIGN_FILL);

  /* Match the bind handler's class assignment — measurement must
   * include the CSS-driven margins / line-height. */
  switch (fw_block_get_kind (block)) {
    case FW_BLOCK_HEADING: {
      gtk_widget_add_css_class (GTK_WIDGET (label), "reflow-heading");
      g_autofree char *cls = g_strdup_printf ("reflow-h%d",
                                              CLAMP (fw_block_get_level (block), 1, 6));
      gtk_widget_add_css_class (GTK_WIDGET (label), cls);
      gtk_label_set_use_markup (label, TRUE);
      gtk_label_set_markup     (label, fw_block_get_text (block) ?: "");
      break;
    }
    case FW_BLOCK_CODE:
      gtk_widget_add_css_class (GTK_WIDGET (label), "reflow-code");
      gtk_label_set_text (label, fw_block_get_text (block) ?: "");
      break;
    case FW_BLOCK_BLOCKQUOTE:
      gtk_widget_add_css_class (GTK_WIDGET (label), "reflow-blockquote");
      gtk_label_set_use_markup (label, TRUE);
      gtk_label_set_markup     (label, fw_block_get_text (block) ?: "");
      break;
    case FW_BLOCK_HR:
      gtk_label_set_text (label, "———");
      break;
    case FW_BLOCK_CHAPTER:
      gtk_widget_add_css_class (GTK_WIDGET (label), "reflow-chapter");
      gtk_label_set_text (label, fw_block_get_text (block) ?: "");
      break;
    default:
      gtk_widget_add_css_class (GTK_WIDGET (label), "reflow-paragraph");
      gtk_label_set_use_markup (label, TRUE);
      gtk_label_set_markup     (label, fw_block_get_text (block) ?: "");
      break;
  }
  return GTK_WIDGET (label);
}

static int
measure_block_height (FwReflowView *self, FwBlock *block, int width)
{
  GtkWidget *m = make_measure_widget_for (self, block);
  /* Sink the floating ref so we own it. */
  g_object_ref_sink (m);

  int min_h = 0, nat_h = 0;
  gtk_widget_measure (m, GTK_ORIENTATION_VERTICAL, width,
                      &min_h, &nat_h, NULL, NULL);

  g_object_unref (m);
  return nat_h;
}

static void
emit_page_changed (FwReflowView *self)
{
  g_signal_emit (self, signals[SIG_PAGE_CHANGED], 0,
                 self->current_page,
                 (guint) (self->pages ? self->pages->len : 0));
}

static void
apply_current_page (FwReflowView *self)
{
  if (!self->page_slice) return;

  if (!self->pages || self->pages->len == 0) {
    gtk_slice_list_model_set_offset (self->page_slice, 0);
    gtk_slice_list_model_set_size   (self->page_slice, 0);
    emit_page_changed (self);
    return;
  }

  if (self->current_page >= self->pages->len)
    self->current_page = self->pages->len - 1;

  FwPageRange r = g_array_index (self->pages, FwPageRange, self->current_page);
  gtk_slice_list_model_set_offset (self->page_slice, r.first);
  gtk_slice_list_model_set_size   (self->page_slice, r.count);

  /* Reset scroll to top of the page (in case content overflows the
   * viewport — rare but possible for a single very tall block). */
  if (self->scroll) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scroll);
    if (vadj) gtk_adjustment_set_value (vadj, 0);
  }

  emit_page_changed (self);
}

static void
recompute_pagination (FwReflowView *self)
{
  if (!self->pages) return;

  int vw = gtk_widget_get_width  (GTK_WIDGET (self->scroll));
  int vh = gtk_widget_get_height (GTK_WIDGET (self->scroll));

  /* Allocation not yet settled — bail. The size_allocate hook will
   * fire again with real dimensions. */
  if (vw < 200 || vh < 200) return;

  int content_w = vw - 2 * READING_COLUMN_MARGIN;
  if (content_w < 100) content_w = vw;
  int page_h = vh - 24;   /* small breathing room */
  if (page_h < 200) page_h = vh;

  /* Remember which block the current page starts on so we can land
   * back at approximately the same content after re-pagination. */
  guint anchor_block = 0;
  if (self->pages->len > 0 && self->current_page < self->pages->len) {
    anchor_block =
      g_array_index (self->pages, FwPageRange, self->current_page).first;
  }

  g_array_set_size (self->pages, 0);

  guint n = self->all_blocks ? g_list_model_get_n_items (self->all_blocks) : 0;
  if (n == 0) {
    self->current_page = 0;
    apply_current_page (self);
    return;
  }

  guint page_start = 0;
  int   page_acc   = 0;

  for (guint i = 0; i < n; i++) {
    g_autoptr (FwBlock) b = g_list_model_get_item (self->all_blocks, i);
    int h = measure_block_height (self, b, content_w);
    if (h <= 0) h = 1;

    if (i > page_start && page_acc + h > page_h) {
      FwPageRange r = { .first = page_start, .count = i - page_start };
      g_array_append_val (self->pages, r);
      page_start = i;
      page_acc   = h;
    } else {
      page_acc += h;
    }
  }
  if (page_start < n) {
    FwPageRange r = { .first = page_start, .count = n - page_start };
    g_array_append_val (self->pages, r);
  }

  /* Land on the page containing the anchor block. */
  guint new_page = 0;
  for (guint p = 0; p < self->pages->len; p++) {
    FwPageRange r = g_array_index (self->pages, FwPageRange, p);
    if (anchor_block >= r.first && anchor_block < r.first + r.count) {
      new_page = p;
      break;
    }
  }
  self->current_page = new_page;

  apply_current_page (self);
}

static gboolean
repaginate_idle_cb (gpointer user_data)
{
  FwReflowView *self = FW_REFLOW_VIEW (user_data);
  self->repaginate_idle = 0;
  recompute_pagination (self);
  return G_SOURCE_REMOVE;
}

static void
queue_repaginate (FwReflowView *self)
{
  if (self->repaginate_idle) return;
  self->repaginate_idle = g_idle_add (repaginate_idle_cb, self);
}

/* ── Public API ───────────────────────────────────────────────────── */

void
fw_reflow_view_set_document (FwReflowView *self, FwReflowDocument *doc)
{
  g_return_if_fail (FW_IS_REFLOW_VIEW (self));

  if (self->document == doc)
    return;

  g_set_object (&self->document, doc);
  self->all_blocks = doc ? fw_reflow_document_get_block_model (doc) : NULL;
  self->current_page = 0;

  if (self->page_slice) {
    /* Set the slice's underlying model to all_blocks. The slice
     * windows it via offset/size, controlled by apply_current_page. */
    gtk_slice_list_model_set_model (self->page_slice, self->all_blocks);
    gtk_slice_list_model_set_offset (self->page_slice, 0);
    gtk_slice_list_model_set_size   (self->page_slice, 0);
  }

  if (self->pages)
    g_array_set_size (self->pages, 0);

  queue_repaginate (self);
}

void
fw_reflow_view_scroll_to_anchor (FwReflowView *self, const char *anchor)
{
  g_return_if_fail (FW_IS_REFLOW_VIEW (self));
  if (!self->document || !anchor || !*anchor)
    return;

  guint pos1 = fw_reflow_document_find_block_by_anchor (self->document, anchor);
  if (pos1 == 0)   /* 1-based — 0 = not found */
    return;
  guint block = pos1 - 1;

  /* Find the page containing this block. */
  if (!self->pages) return;
  for (guint p = 0; p < self->pages->len; p++) {
    FwPageRange r = g_array_index (self->pages, FwPageRange, p);
    if (block >= r.first && block < r.first + r.count) {
      self->current_page = p;
      apply_current_page (self);
      return;
    }
  }
}

void
fw_reflow_view_scroll_by_page (FwReflowView *self, int direction)
{
  g_return_if_fail (FW_IS_REFLOW_VIEW (self));
  if (!self->pages || self->pages->len == 0)
    return;

  guint last = self->pages->len - 1;
  guint next = self->current_page;

  if (direction == 0)             next = 0;
  else if (direction == G_MAXINT) next = last;
  else if (direction > 0)         next = MIN (self->current_page + 1, last);
  else if (direction < 0)         next = (self->current_page > 0)
                                          ? self->current_page - 1 : 0;

  if (next == self->current_page)
    return;
  self->current_page = next;
  apply_current_page (self);
}

guint
fw_reflow_view_get_current_page (FwReflowView *self)
{
  g_return_val_if_fail (FW_IS_REFLOW_VIEW (self), 0);
  return self->current_page;
}

guint
fw_reflow_view_get_total_pages (FwReflowView *self)
{
  g_return_val_if_fail (FW_IS_REFLOW_VIEW (self), 0);
  return self->pages ? self->pages->len : 0;
}

/* ── GObject lifecycle ────────────────────────────────────────────── */

static void
fw_reflow_view_size_allocate (GtkWidget *widget, int width, int height,
                              int baseline)
{
  FwReflowView *self = FW_REFLOW_VIEW (widget);

  GTK_WIDGET_CLASS (fw_reflow_view_parent_class)
    ->size_allocate (widget, width, height, baseline);

  if (width != self->last_alloc_w || height != self->last_alloc_h) {
    self->last_alloc_w = width;
    self->last_alloc_h = height;
    queue_repaginate (self);
  }
}

static void
fw_reflow_view_dispose (GObject *object)
{
  FwReflowView *self = FW_REFLOW_VIEW (object);

  if (self->repaginate_idle) {
    g_source_remove (self->repaginate_idle);
    self->repaginate_idle = 0;
  }

  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    gtk_widget_unparent (child);
    child = next;
  }

  if (self->settings && self->settings_handler) {
    g_signal_handler_disconnect (self->settings, self->settings_handler);
    self->settings_handler = 0;
  }
  g_clear_object (&self->settings);
  g_clear_object (&self->document);
  g_clear_object (&self->css);
  g_clear_object (&self->selection);
  g_clear_object (&self->page_slice);
  self->all_blocks = NULL;
  g_clear_pointer (&self->pages, g_array_unref);

  G_OBJECT_CLASS (fw_reflow_view_parent_class)->dispose (object);
}

static void
fw_reflow_view_class_init (FwReflowViewClass *klass)
{
  GObjectClass   *o_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *w_class = GTK_WIDGET_CLASS (klass);

  o_class->dispose       = fw_reflow_view_dispose;
  w_class->size_allocate = fw_reflow_view_size_allocate;

  gtk_widget_class_set_layout_manager_type (w_class, GTK_TYPE_BIN_LAYOUT);

  signals[SIG_PAGE_CHANGED] =
    g_signal_new ("page-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
}

static void
fw_reflow_view_init (FwReflowView *self)
{
  ensure_css (self);

  self->pages         = g_array_new (FALSE, FALSE, sizeof (FwPageRange));
  self->current_page  = 0;
  self->last_alloc_w  = 0;
  self->last_alloc_h  = 0;

  /* Slice wraps an initially-empty model. set_document swaps in the
   * doc's block model; apply_current_page sets offset/size each page. */
  GListStore *empty = g_list_store_new (FW_TYPE_BLOCK);
  self->page_slice = gtk_slice_list_model_new (G_LIST_MODEL (empty), 0, 0);
  /* gtk_slice_list_model_new takes ownership of `empty`. */

  self->selection = GTK_SINGLE_SELECTION (
    gtk_single_selection_new (G_LIST_MODEL (g_object_ref (self->page_slice))));
  gtk_single_selection_set_can_unselect (self->selection, TRUE);
  gtk_single_selection_set_autoselect    (self->selection, FALSE);

  GtkSignalListItemFactory *factory =
    GTK_SIGNAL_LIST_ITEM_FACTORY (gtk_signal_list_item_factory_new ());
  g_signal_connect (factory, "setup", G_CALLBACK (on_factory_setup), self);
  g_signal_connect (factory, "bind",  G_CALLBACK (on_factory_bind),  self);

  /* gtk_list_view_new takes ownership (transfer-full) of both the
   * selection model and the factory — do NOT unref after. */
  self->list = GTK_LIST_VIEW (gtk_list_view_new (
    GTK_SELECTION_MODEL (g_object_ref (self->selection)),
    GTK_LIST_ITEM_FACTORY (factory)));
  gtk_list_view_set_show_separators (self->list, FALSE);
  gtk_list_view_set_single_click_activate (self->list, FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->list), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "reflow-listview");

  self->scroll = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_child (self->scroll, GTK_WIDGET (self->list));
  gtk_scrolled_window_set_policy (self->scroll,
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_width (self->scroll,
                                              READING_COLUMN_MAX_WIDTH);
  gtk_scrolled_window_set_propagate_natural_width (self->scroll, FALSE);

  gtk_widget_set_hexpand (GTK_WIDGET (self->scroll), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->scroll), TRUE);
  gtk_widget_set_parent  (GTK_WIDGET (self->scroll), GTK_WIDGET (self));
}

FwReflowView *
fw_reflow_view_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_VIEW, NULL);
}
