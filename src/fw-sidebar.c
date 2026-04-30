/* fw-sidebar.c — TOC sidebar (GtkListView + GtkTreeListModel)
 *
 * Migrated from the deprecated GtkTreeView path. The model is a hand-built
 * tree of FwTocItem GObjects mirroring the FwTocNode structure: each item
 * holds its title, destination page, and a child GListStore. A
 * GtkTreeListModel wraps the root store and, on demand, asks each item
 * for its children GListStore — which lets us avoid expanding the whole
 * tree up front.
 *
 * Current-page highlight: walks the underlying FwTocItem tree (not the
 * flat tree-list-model) for the deepest match, expands every ancestor
 * row to make the target reachable, then walks the flat model to find
 * the matching row position for selection + scroll-to-cell.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-sidebar.h"

/* ── FwTocItem GObject ────────────────────────────────────────────── */

#define FW_TYPE_TOC_ITEM (fw_toc_item_get_type ())
G_DECLARE_FINAL_TYPE (FwTocItem, fw_toc_item, FW, TOC_ITEM, GObject)

struct _FwTocItem {
  GObject       parent_instance;
  char         *title;
  int           page;            /* destination page, -1 if none */
  GListStore   *children;        /* may be NULL if leaf */
};

G_DEFINE_FINAL_TYPE (FwTocItem, fw_toc_item, G_TYPE_OBJECT)

static void
fw_toc_item_finalize (GObject *object)
{
  FwTocItem *self = FW_TOC_ITEM (object);
  g_free (self->title);
  g_clear_object (&self->children);
  G_OBJECT_CLASS (fw_toc_item_parent_class)->finalize (object);
}

static void
fw_toc_item_class_init (FwTocItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fw_toc_item_finalize;
}

static void
fw_toc_item_init (FwTocItem *self)
{
  self->page = -1;
}

static FwTocItem *
fw_toc_item_new (const char *title, int page)
{
  FwTocItem *item = g_object_new (FW_TYPE_TOC_ITEM, NULL);
  item->title = g_strdup (title ? title : "");
  item->page  = page;
  return item;
}

/* ── Sidebar ──────────────────────────────────────────────────────── */

struct _FwSidebar {
  GtkWidget         parent_instance;
  GtkScrolledWindow *scroll;
  GtkListView       *list_view;
  GtkLabel          *placeholder;
  GListStore        *root_store;        /* of FwTocItem; only top-level */
  GtkTreeListModel  *tree_model;        /* wraps root_store, lazy-expand */
  GtkSingleSelection *selection;
  int                highlighted_page;
  gboolean           suppress_select_signal;
};

G_DEFINE_FINAL_TYPE (FwSidebar, fw_sidebar, GTK_TYPE_WIDGET)

enum {
  SIGNAL_PAGE_REQUESTED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* ── Tree construction ───────────────────────────────────────────── */

static void
populate_store (GListStore *store, const FwTocNode *node)
{
  for (const FwTocNode *n = node; n; n = n->next) {
    FwTocItem *item = fw_toc_item_new (n->title, n->page);
    if (n->children) {
      item->children = g_list_store_new (FW_TYPE_TOC_ITEM);
      populate_store (item->children, n->children);
    }
    g_list_store_append (store, item);
    g_object_unref (item);
  }
}

/* GtkTreeListModelCreateModelFunc — given an item, return a GListModel
 * of its children, or NULL if it's a leaf. The TreeListModel takes a
 * reference to the returned model. */
static GListModel *
create_child_model (gpointer item, gpointer user_data)
{
  (void) user_data;
  FwTocItem *toc = FW_TOC_ITEM (item);
  if (!toc->children)
    return NULL;
  return G_LIST_MODEL (g_object_ref (toc->children));
}

/* ── ListItem factory ────────────────────────────────────────────── */

static void
factory_setup (GtkSignalListItemFactory *factory, GtkListItem *list_item,
               gpointer user_data)
{
  (void) factory; (void) user_data;
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);

  GtkWidget *expander = gtk_tree_expander_new ();
  gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), label);
  gtk_list_item_set_child (list_item, expander);
}

static void
factory_bind (GtkSignalListItemFactory *factory, GtkListItem *list_item,
              gpointer user_data)
{
  (void) factory; (void) user_data;
  GtkTreeExpander *expander = GTK_TREE_EXPANDER (
    gtk_list_item_get_child (list_item));
  GtkTreeListRow *row = GTK_TREE_LIST_ROW (
    gtk_list_item_get_item (list_item));

  gtk_tree_expander_set_list_row (expander, row);

  FwTocItem *item = FW_TOC_ITEM (gtk_tree_list_row_get_item (row));
  GtkLabel *label = GTK_LABEL (gtk_tree_expander_get_child (expander));
  gtk_label_set_text (label, item->title);
  g_object_unref (item);  /* gtk_tree_list_row_get_item gives a ref */
}

/* ── Activation (row click) ──────────────────────────────────────── */

static void
on_list_view_activate (GtkListView *list_view, guint position,
                       gpointer user_data)
{
  (void) list_view;
  FwSidebar *self = FW_SIDEBAR (user_data);

  if (!self->tree_model)
    return;

  GtkTreeListRow *row =
    g_list_model_get_item (G_LIST_MODEL (self->tree_model), position);
  if (!row)
    return;
  FwTocItem *item = FW_TOC_ITEM (gtk_tree_list_row_get_item (row));
  if (item->page >= 0)
    g_signal_emit (self, signals[SIGNAL_PAGE_REQUESTED], 0, item->page);
  g_object_unref (item);
  g_object_unref (row);
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

  self->highlighted_page = -1;

  if (!toc) {
    g_list_store_remove_all (self->root_store);
    gtk_widget_set_visible (GTK_WIDGET (self->scroll),     FALSE);
    gtk_widget_set_visible (GTK_WIDGET (self->placeholder), TRUE);
    return;
  }

  g_list_store_remove_all (self->root_store);
  populate_store (self->root_store, toc);

  gtk_widget_set_visible (GTK_WIDGET (self->scroll),     TRUE);
  gtk_widget_set_visible (GTK_WIDGET (self->placeholder), FALSE);
}

/* Recursive walker over the underlying FwTocItem tree (not the flat
 * tree-list-model). Returns the deepest item whose page ≤ current_page
 * (or NULL if no match). best_item is borrowed — caller must not unref. */
static FwTocItem *
find_best_match (GListStore *store, int current_page, int *best_page)
{
  FwTocItem *best = NULL;
  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    FwTocItem *item = FW_TOC_ITEM (g_list_model_get_item (G_LIST_MODEL (store), i));
    if (item->page >= 0 && item->page <= current_page && item->page > *best_page) {
      *best_page = item->page;
      best = item;
    }
    if (item->children) {
      FwTocItem *deeper = find_best_match (item->children, current_page, best_page);
      if (deeper)
        best = deeper;
    }
    g_object_unref (item);
  }
  return best;
}

/* Build a parent path (top-down list of FwTocItem*) from root_store to
 * the target. Used to expand ancestors before selecting. Returns TRUE
 * on success. The caller must g_array_unref the returned array. */
static gboolean
build_path (GListStore *store, FwTocItem *target, GArray *path)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (guint i = 0; i < n; i++) {
    FwTocItem *item = FW_TOC_ITEM (g_list_model_get_item (G_LIST_MODEL (store), i));
    if (item == target) {
      g_array_append_val (path, item);
      g_object_unref (item);
      return TRUE;
    }
    if (item->children && build_path (item->children, target, path)) {
      g_array_prepend_val (path, item);
      g_object_unref (item);
      return TRUE;
    }
    g_object_unref (item);
  }
  return FALSE;
}

void
fw_sidebar_set_current_page (FwSidebar *self, int current_page)
{
  g_return_if_fail (FW_IS_SIDEBAR (self));

  if (self->highlighted_page == current_page)
    return;
  self->highlighted_page = current_page;

  if (!self->tree_model || !self->root_store ||
      g_list_model_get_n_items (G_LIST_MODEL (self->root_store)) == 0)
    return;

  int best_page = -1;
  FwTocItem *target =
    find_best_match (self->root_store, current_page, &best_page);
  if (!target)
    return;

  /* Expand ancestors so target is reachable in the flat model. */
  GArray *path = g_array_new (FALSE, FALSE, sizeof (FwTocItem *));
  if (!build_path (self->root_store, target, path)) {
    g_array_unref (path);
    return;
  }

  /* Walk down the path expanding each non-leaf ancestor's row. We do
   * this by finding each ancestor in the *current* flat model (which
   * grows as we expand), then setting its row to expanded. */
  for (guint depth = 0; depth + 1 < path->len; depth++) {
    FwTocItem *ancestor = g_array_index (path, FwTocItem *, depth);
    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->tree_model));
    for (guint i = 0; i < n; i++) {
      GtkTreeListRow *row = g_list_model_get_item (
        G_LIST_MODEL (self->tree_model), i);
      FwTocItem *it = FW_TOC_ITEM (gtk_tree_list_row_get_item (row));
      if (it == ancestor && !gtk_tree_list_row_get_expanded (row))
        gtk_tree_list_row_set_expanded (row, TRUE);
      g_object_unref (it);
      g_object_unref (row);
      if (it == ancestor)
        break;
    }
  }

  /* Find target's flat position and select + scroll. */
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->tree_model));
  for (guint i = 0; i < n; i++) {
    GtkTreeListRow *row = g_list_model_get_item (
      G_LIST_MODEL (self->tree_model), i);
    FwTocItem *it = FW_TOC_ITEM (gtk_tree_list_row_get_item (row));
    if (it == target) {
      self->suppress_select_signal = TRUE;
      gtk_single_selection_set_selected (self->selection, i);
      gtk_list_view_scroll_to (self->list_view, i, GTK_LIST_SCROLL_NONE, NULL);
      self->suppress_select_signal = FALSE;
      g_object_unref (it);
      g_object_unref (row);
      break;
    }
    g_object_unref (it);
    g_object_unref (row);
  }

  g_array_unref (path);
}

/* ── GtkWidget vfuncs (forwarding to the active visible child) ──── */

static void
fw_sidebar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  FwSidebar *self = FW_SIDEBAR (widget);
  if (gtk_widget_get_visible (GTK_WIDGET (self->scroll)))
    gtk_widget_snapshot_child (widget, GTK_WIDGET (self->scroll), snapshot);
  else
    gtk_widget_snapshot_child (widget, GTK_WIDGET (self->placeholder), snapshot);
}

static void
fw_sidebar_measure (GtkWidget *widget, GtkOrientation orientation,
                    int for_size, int *minimum, int *natural,
                    int *minimum_baseline, int *natural_baseline)
{
  FwSidebar *self = FW_SIDEBAR (widget);
  GtkWidget *child = gtk_widget_get_visible (GTK_WIDGET (self->scroll))
    ? GTK_WIDGET (self->scroll)
    : GTK_WIDGET (self->placeholder);
  gtk_widget_measure (child, orientation, for_size,
                      minimum, natural, minimum_baseline, natural_baseline);
}

static void
fw_sidebar_size_allocate (GtkWidget *widget, int width, int height,
                          int baseline)
{
  FwSidebar *self = FW_SIDEBAR (widget);
  GtkAllocation alloc = { 0, 0, width, height };
  if (gtk_widget_get_visible (GTK_WIDGET (self->scroll)))
    gtk_widget_size_allocate (GTK_WIDGET (self->scroll), &alloc, baseline);
  if (gtk_widget_get_visible (GTK_WIDGET (self->placeholder)))
    gtk_widget_size_allocate (GTK_WIDGET (self->placeholder), &alloc, baseline);
}

static void
fw_sidebar_dispose (GObject *object)
{
  FwSidebar *self = FW_SIDEBAR (object);
  g_clear_pointer ((GtkWidget **) &self->scroll,      gtk_widget_unparent);
  g_clear_pointer ((GtkWidget **) &self->placeholder, gtk_widget_unparent);
  G_OBJECT_CLASS (fw_sidebar_parent_class)->dispose (object);
}

static void
fw_sidebar_class_init (FwSidebarClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose       = fw_sidebar_dispose;
  widget_class->snapshot      = fw_sidebar_snapshot;
  widget_class->measure       = fw_sidebar_measure;
  widget_class->size_allocate = fw_sidebar_size_allocate;

  signals[SIGNAL_PAGE_REQUESTED] =
    g_signal_new ("page-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
fw_sidebar_init (FwSidebar *self)
{
  self->highlighted_page = -1;

  /* Model stack: root_store → tree_model (lazy children) → selection */
  self->root_store = g_list_store_new (FW_TYPE_TOC_ITEM);
  self->tree_model = gtk_tree_list_model_new (
    G_LIST_MODEL (g_object_ref (self->root_store)),
    FALSE,                       /* passthrough */
    FALSE,                       /* autoexpand */
    create_child_model, self, NULL);
  self->selection = gtk_single_selection_new (
    G_LIST_MODEL (g_object_ref (self->tree_model)));
  gtk_single_selection_set_can_unselect (self->selection, TRUE);
  gtk_single_selection_set_autoselect    (self->selection, FALSE);

  /* Factory: tree expander + label */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (factory_setup), self);
  g_signal_connect (factory, "bind",  G_CALLBACK (factory_bind),  self);

  self->list_view = GTK_LIST_VIEW (gtk_list_view_new (
    GTK_SELECTION_MODEL (self->selection), factory));
  gtk_list_view_set_show_separators (self->list_view, FALSE);
  gtk_list_view_set_single_click_activate (self->list_view, FALSE);
  g_signal_connect (self->list_view, "activate",
                    G_CALLBACK (on_list_view_activate), self);

  self->scroll = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_child (self->scroll, GTK_WIDGET (self->list_view));
  gtk_widget_set_parent (GTK_WIDGET (self->scroll), GTK_WIDGET (self));
  gtk_widget_set_visible (GTK_WIDGET (self->scroll), FALSE);

  self->placeholder = GTK_LABEL (gtk_label_new ("No table of contents"));
  gtk_widget_add_css_class (GTK_WIDGET (self->placeholder), "dim-label");
  gtk_widget_set_parent (GTK_WIDGET (self->placeholder), GTK_WIDGET (self));
  gtk_widget_set_visible (GTK_WIDGET (self->placeholder), TRUE);
}
