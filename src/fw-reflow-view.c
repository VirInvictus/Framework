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

static GtkWidget *
make_paragraph_label (void)
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
  gtk_widget_set_size_request  (GTK_WIDGET (label), -1, -1);
  gtk_widget_add_css_class     (GTK_WIDGET (label), "reflow-paragraph");
  return GTK_WIDGET (label);
}

static void
on_factory_setup (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                  GObject                  *listitem,
                  gpointer                  user_data G_GNUC_UNUSED)
{
  /* One label that switches role at bind time based on FwBlock kind.
   * Cheap default: paragraph styling. Headings/code/HR rebind props on
   * bind. Cleaner long-term is per-kind widgets in a GtkStack — when
   * EPUB lands and needs GtkPicture for images, that's the upgrade. */
  GtkWidget *label = make_paragraph_label ();
  gtk_list_item_set_child (GTK_LIST_ITEM (listitem), label);
}

static void
on_factory_bind (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                 GObject                  *listitem,
                 gpointer                  user_data G_GNUC_UNUSED)
{
  GtkListItem *item   = GTK_LIST_ITEM (listitem);
  GtkWidget   *child  = gtk_list_item_get_child (item);
  FwBlock     *block  = FW_BLOCK (gtk_list_item_get_item (item));

  if (!child || !block)
    return;

  /* Reset state that bind sites might mutate. */
  gtk_widget_remove_css_class (child, "reflow-heading");
  gtk_widget_remove_css_class (child, "reflow-code");
  gtk_widget_remove_css_class (child, "reflow-blockquote");
  gtk_widget_remove_css_class (child, "reflow-chapter");

  GtkLabel *label = GTK_LABEL (child);

  switch (fw_block_get_kind (block)) {
    case FW_BLOCK_HEADING: {
      gtk_widget_add_css_class (child, "reflow-heading");
      g_autofree char *cls = g_strdup_printf ("reflow-h%d",
                                              CLAMP (fw_block_get_level (block), 1, 6));
      gtk_widget_add_css_class (child, cls);
      gtk_label_set_use_markup (label, TRUE);
      gtk_label_set_markup (label, fw_block_get_text (block) ?: "");
      break;
    }
    case FW_BLOCK_CODE:
      gtk_widget_add_css_class (child, "reflow-code");
      gtk_label_set_use_markup (label, FALSE);
      gtk_label_set_text (label, fw_block_get_text (block) ?: "");
      break;
    case FW_BLOCK_BLOCKQUOTE:
      gtk_widget_add_css_class (child, "reflow-blockquote");
      gtk_label_set_use_markup (label, TRUE);
      gtk_label_set_markup (label, fw_block_get_text (block) ?: "");
      break;
    case FW_BLOCK_HR:
      /* Render as a thin separator-style line of glyphs. Trivial
       * placeholder until FwReflowView gets a real per-kind factory. */
      gtk_label_set_use_markup (label, FALSE);
      gtk_label_set_text (label, "———");
      break;
    case FW_BLOCK_CHAPTER:
      gtk_widget_add_css_class (child, "reflow-chapter");
      gtk_label_set_use_markup (label, FALSE);
      gtk_label_set_text (label, fw_block_get_text (block) ?: "");
      break;
    case FW_BLOCK_IMAGE:
    case FW_BLOCK_LIST:
    case FW_BLOCK_LIST_ITEM:
    case FW_BLOCK_PARAGRAPH:
    default:
      gtk_label_set_use_markup (label, TRUE);
      gtk_label_set_markup (label, fw_block_get_text (block) ?: "");
      break;
  }
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
  ".reflow-chapter   { font-weight: bold; opacity: 0.6; }";

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
