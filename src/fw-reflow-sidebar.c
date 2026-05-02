/* fw-reflow-sidebar.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-sidebar.h"

struct _FwReflowSidebar {
  GtkWidget          parent_instance;
  GtkScrolledWindow *scroll;
  GtkListView       *list;
  GtkSingleSelection *selection;
  GListModel        *toc_model;   /* unowned reference */
};

enum {
  SIG_ANCHOR_REQUESTED,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (FwReflowSidebar, fw_reflow_sidebar, GTK_TYPE_WIDGET)

/* ── Factory ──────────────────────────────────────────────────────── */

static void
on_setup (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
          GObject                  *listitem,
          gpointer                  user_data G_GNUC_UNUSED)
{
  GtkLabel *label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_xalign (label, 0.0);
  gtk_label_set_ellipsize (label, PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start  (GTK_WIDGET (label), 12);
  gtk_widget_set_margin_end    (GTK_WIDGET (label), 12);
  gtk_widget_set_margin_top    (GTK_WIDGET (label), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (label), 6);
  gtk_list_item_set_child (GTK_LIST_ITEM (listitem), GTK_WIDGET (label));
}

static void
on_bind (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
         GObject                  *listitem,
         gpointer                  user_data G_GNUC_UNUSED)
{
  GtkListItem      *item  = GTK_LIST_ITEM (listitem);
  GtkWidget        *child = gtk_list_item_get_child (item);
  FwReflowTocItem  *tocit = FW_REFLOW_TOC_ITEM (gtk_list_item_get_item (item));
  if (!child || !tocit) return;

  gtk_label_set_text (GTK_LABEL (child),
                      fw_reflow_toc_item_get_title (tocit) ?: "");
}

/* ── Activation → signal ──────────────────────────────────────────── */

static void
on_activate (GtkListView *list,
             guint        position,
             gpointer     user_data)
{
  FwReflowSidebar *self = FW_REFLOW_SIDEBAR (user_data);
  if (!self->toc_model)
    return;
  g_autoptr (FwReflowTocItem) tocit =
    g_list_model_get_item (self->toc_model, position);
  if (!tocit)
    return;
  const char *anchor = fw_reflow_toc_item_get_anchor_id (tocit);
  if (anchor && *anchor)
    g_signal_emit (self, signals[SIG_ANCHOR_REQUESTED], 0, anchor);
  (void) list;
}

/* ── Public API ───────────────────────────────────────────────────── */

void
fw_reflow_sidebar_set_toc (FwReflowSidebar *self, GListModel *toc_model)
{
  g_return_if_fail (FW_IS_REFLOW_SIDEBAR (self));

  self->toc_model = toc_model;   /* unowned — selection takes its own ref */

  if (self->selection)
    gtk_single_selection_set_model (self->selection, toc_model);
}

/* ── GObject lifecycle ────────────────────────────────────────────── */

static void
fw_reflow_sidebar_dispose (GObject *object)
{
  FwReflowSidebar *self = FW_REFLOW_SIDEBAR (object);

  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    gtk_widget_unparent (child);
    child = next;
  }
  self->toc_model = NULL;

  G_OBJECT_CLASS (fw_reflow_sidebar_parent_class)->dispose (object);
}

static void
fw_reflow_sidebar_class_init (FwReflowSidebarClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = fw_reflow_sidebar_dispose;
  gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass),
                                            GTK_TYPE_BIN_LAYOUT);

  signals[SIG_ANCHOR_REQUESTED] =
    g_signal_new ("anchor-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
fw_reflow_sidebar_init (FwReflowSidebar *self)
{
  GListStore *empty = g_list_store_new (FW_TYPE_REFLOW_TOC_ITEM);
  self->selection = GTK_SINGLE_SELECTION (
    gtk_single_selection_new (G_LIST_MODEL (empty)));
  gtk_single_selection_set_can_unselect (self->selection, TRUE);
  gtk_single_selection_set_autoselect    (self->selection, FALSE);

  GtkSignalListItemFactory *factory =
    GTK_SIGNAL_LIST_ITEM_FACTORY (gtk_signal_list_item_factory_new ());
  g_signal_connect (factory, "setup", G_CALLBACK (on_setup), NULL);
  g_signal_connect (factory, "bind",  G_CALLBACK (on_bind),  NULL);

  self->list = GTK_LIST_VIEW (gtk_list_view_new (
    GTK_SELECTION_MODEL (g_object_ref (self->selection)),
    GTK_LIST_ITEM_FACTORY (factory)));
  gtk_list_view_set_show_separators (self->list, FALSE);
  gtk_list_view_set_single_click_activate (self->list, TRUE);
  g_signal_connect (self->list, "activate", G_CALLBACK (on_activate), self);

  self->scroll = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_child (self->scroll, GTK_WIDGET (self->list));
  gtk_scrolled_window_set_policy (self->scroll,
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand (GTK_WIDGET (self->scroll), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->scroll), TRUE);
  gtk_widget_set_parent  (GTK_WIDGET (self->scroll), GTK_WIDGET (self));
}

FwReflowSidebar *
fw_reflow_sidebar_new (void)
{
  return g_object_new (FW_TYPE_REFLOW_SIDEBAR, NULL);
}
