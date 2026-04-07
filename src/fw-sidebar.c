/* fw-sidebar.c — TOC sidebar (tree view)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-sidebar.h"

struct _FwSidebar {
  GtkWidget    parent_instance;
  GtkTreeView *tree_view;
  GtkLabel    *placeholder;
};

G_DEFINE_FINAL_TYPE (FwSidebar, fw_sidebar, GTK_TYPE_WIDGET)

enum {
  COL_TITLE,
  COL_PAGE,
  N_COLS,
};

/* ── Populate tree store from TOC nodes ───────────────────────────── */

static void
populate_store (GtkTreeStore    *store,
                GtkTreeIter     *parent,
                const FwTocNode *node)
{
  for (const FwTocNode *n = node; n; n = n->next) {
    GtkTreeIter iter;
    gtk_tree_store_append (store, &iter, parent);
    gtk_tree_store_set (store, &iter,
                        COL_TITLE, n->title,
                        COL_PAGE, n->page,
                        -1);
    if (n->children)
      populate_store (store, &iter, n->children);
  }
}

/* ── Public API ───────────────────────────────────────────────────── */

FwSidebar *
fw_sidebar_new (void)
{
  return g_object_new (FW_TYPE_SIDEBAR, NULL);
}

void
fw_sidebar_set_toc (FwSidebar *self, const FwTocNode *toc)
{
  g_return_if_fail (FW_IS_SIDEBAR (self));

  if (!toc) {
    gtk_widget_set_visible (GTK_WIDGET (self->tree_view), FALSE);
    gtk_widget_set_visible (GTK_WIDGET (self->placeholder), TRUE);
    return;
  }

  GtkTreeStore *store = gtk_tree_store_new (N_COLS,
                                             G_TYPE_STRING,
                                             G_TYPE_INT);
  populate_store (store, NULL, toc);
  gtk_tree_view_set_model (self->tree_view, GTK_TREE_MODEL (store));
  g_object_unref (store);

  gtk_widget_set_visible (GTK_WIDGET (self->tree_view), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->placeholder), FALSE);
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_sidebar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  FwSidebar *self = FW_SIDEBAR (widget);

  /* Snapshot visible child */
  if (gtk_widget_get_visible (GTK_WIDGET (self->tree_view)))
    gtk_widget_snapshot_child (widget, GTK_WIDGET (self->tree_view), snapshot);
  else
    gtk_widget_snapshot_child (widget, GTK_WIDGET (self->placeholder), snapshot);
}

static void
fw_sidebar_measure (GtkWidget      *widget,
                    GtkOrientation  orientation,
                    int             for_size,
                    int            *minimum,
                    int            *natural,
                    int            *minimum_baseline,
                    int            *natural_baseline)
{
  FwSidebar *self = FW_SIDEBAR (widget);
  GtkWidget *child;

  if (gtk_widget_get_visible (GTK_WIDGET (self->tree_view)))
    child = GTK_WIDGET (self->tree_view);
  else
    child = GTK_WIDGET (self->placeholder);

  gtk_widget_measure (child, orientation, for_size,
                      minimum, natural, minimum_baseline, natural_baseline);
}

static void
fw_sidebar_size_allocate (GtkWidget *widget, int width, int height,
                          int baseline)
{
  FwSidebar *self = FW_SIDEBAR (widget);
  GtkAllocation alloc = { 0, 0, width, height };

  if (gtk_widget_get_visible (GTK_WIDGET (self->tree_view)))
    gtk_widget_size_allocate (GTK_WIDGET (self->tree_view), &alloc, baseline);
  if (gtk_widget_get_visible (GTK_WIDGET (self->placeholder)))
    gtk_widget_size_allocate (GTK_WIDGET (self->placeholder), &alloc, baseline);
}

static void
fw_sidebar_dispose (GObject *object)
{
  FwSidebar *self = FW_SIDEBAR (object);

  g_clear_pointer ((GtkWidget **) &self->tree_view,
                   gtk_widget_unparent);
  g_clear_pointer ((GtkWidget **) &self->placeholder,
                   gtk_widget_unparent);

  G_OBJECT_CLASS (fw_sidebar_parent_class)->dispose (object);
}

static void
fw_sidebar_class_init (FwSidebarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose       = fw_sidebar_dispose;
  widget_class->snapshot      = fw_sidebar_snapshot;
  widget_class->measure       = fw_sidebar_measure;
  widget_class->size_allocate = fw_sidebar_size_allocate;
}

static void
fw_sidebar_init (FwSidebar *self)
{
  /* Tree view for TOC */
  self->tree_view = GTK_TREE_VIEW (gtk_tree_view_new ());
  gtk_tree_view_set_headers_visible (self->tree_view, FALSE);

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes (
    "Title", renderer, "text", COL_TITLE, NULL);
  gtk_tree_view_append_column (self->tree_view, col);

  gtk_widget_set_parent (GTK_WIDGET (self->tree_view), GTK_WIDGET (self));
  gtk_widget_set_visible (GTK_WIDGET (self->tree_view), FALSE);

  /* Placeholder label */
  self->placeholder = GTK_LABEL (gtk_label_new ("No table of contents"));
  gtk_widget_add_css_class (GTK_WIDGET (self->placeholder), "dim-label");
  gtk_widget_set_parent (GTK_WIDGET (self->placeholder), GTK_WIDGET (self));
  gtk_widget_set_visible (GTK_WIDGET (self->placeholder), TRUE);
}
