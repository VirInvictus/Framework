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

#define READING_COLUMN_MAX_WIDTH 720   /* px — caps comfortable line length */
#define READING_COLUMN_MARGIN     24

struct _FwReflowView {
  GtkWidget          parent_instance;

  FwReflowDocument  *document;     /* owned */

  GtkScrolledWindow *scroll;       /* fills our area */
  GtkListView       *list;
  GtkSingleSelection *selection;

  GtkCssProvider    *css;
};

G_DEFINE_FINAL_TYPE (FwReflowView, fw_reflow_view, GTK_TYPE_WIDGET)

/* ── Factory: FwBlock → GtkWidget ─────────────────────────────────── */

#define IMAGE_MAX_HEIGHT_PX 600

/* Each row hosts a GtkStack with two named children:
 *   "text"  → a wrapping, selectable GtkLabel for paragraph / heading /
 *             code / blockquote / hr / chapter blocks.
 *   "image" → a GtkPicture with content-fit=contain, capped at
 *             IMAGE_MAX_HEIGHT_PX so a cover image never dominates the
 *             viewport.
 * Bind switches the visible page; the unused widget is preserved so
 * recycled rows skip widget churn when the kind toggles back. */

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
  gtk_widget_set_margin_top    (GTK_WIDGET (label), 4);
  gtk_widget_set_margin_bottom (GTK_WIDGET (label), 4);
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
  gtk_widget_set_margin_top      (GTK_WIDGET (pic), 8);
  gtk_widget_set_margin_bottom   (GTK_WIDGET (pic), 8);
  /* Min-height is the natural floor; combined with content-fit=contain,
   * the picture scales down to fit the row width without ballooning. */
  gtk_widget_set_size_request    (GTK_WIDGET (pic), -1, IMAGE_MAX_HEIGHT_PX);
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

/* ── CSS for typography ───────────────────────────────────────────── */

static const char REFLOW_CSS[] =
  ".reflow-paragraph { font-size: 12pt; line-height: 1.5; }"
  ".reflow-heading   { font-weight: bold; font-size: 14pt; margin-top: 12px; }"
  ".reflow-h1        { font-size: 22pt; }"
  ".reflow-h2        { font-size: 18pt; }"
  ".reflow-h3        { font-size: 15pt; }"
  ".reflow-h4        { font-size: 13pt; }"
  ".reflow-code      { font-family: monospace; font-size: 11pt; }"
  ".reflow-blockquote{ font-style: italic; opacity: 0.8; "
  "                    border-left: 3px solid alpha(currentColor, 0.3); "
  "                    padding-left: 12px; }"
  ".reflow-chapter   { font-weight: bold; opacity: 0.6; }"
  ".reflow-image     { background: alpha(currentColor, 0.04); "
  "                    border-radius: 4px; }";

static void
ensure_css (FwReflowView *self)
{
  if (self->css)
    return;
  self->css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (self->css, REFLOW_CSS);
  gtk_style_context_add_provider_for_display (
    gdk_display_get_default (),
    GTK_STYLE_PROVIDER (self->css),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ── Public API ───────────────────────────────────────────────────── */

void
fw_reflow_view_set_document (FwReflowView *self, FwReflowDocument *doc)
{
  g_return_if_fail (FW_IS_REFLOW_VIEW (self));

  if (self->document == doc)
    return;

  g_set_object (&self->document, doc);

  GListModel *blocks = doc ? fw_reflow_document_get_block_model (doc) : NULL;
  if (self->selection) {
    gtk_single_selection_set_model (self->selection, blocks);
  }
}

/* ── GObject lifecycle ────────────────────────────────────────────── */

static void
fw_reflow_view_dispose (GObject *object)
{
  FwReflowView *self = FW_REFLOW_VIEW (object);

  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    gtk_widget_unparent (child);
    child = next;
  }

  g_clear_object (&self->document);
  g_clear_object (&self->css);

  G_OBJECT_CLASS (fw_reflow_view_parent_class)->dispose (object);
}

static void
fw_reflow_view_class_init (FwReflowViewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = fw_reflow_view_dispose;
  gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass),
                                            GTK_TYPE_BIN_LAYOUT);
}

static void
fw_reflow_view_init (FwReflowView *self)
{
  ensure_css (self);

  /* Empty model bound now so the listview has structure even before
   * a document arrives. */
  GListStore *empty = g_list_store_new (FW_TYPE_BLOCK);
  self->selection = GTK_SINGLE_SELECTION (
    gtk_single_selection_new (G_LIST_MODEL (empty)));
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
