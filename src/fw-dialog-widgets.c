/* fw-dialog-widgets.c — Owned dialog + boxed-list row helpers
 *
 * The libadwaita-free stand-ins for AdwDialog/AdwToolbarView/
 * AdwPreferences*: a modal transient GtkWindow with a flat header and
 * Escape-to-close, and GtkListBox "boxed-list" rows whose padding and
 * typography come from the owned stylesheet. Ported from the Hermitage
 * sibling's widgets.py (v0.80.0); extracted out of fw-window.c so the
 * window file stays navigable.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-dialog-widgets.h"

/* ── Escape-to-close ────────────────────────────────────────────────── */

static gboolean
on_dialog_escape (GtkEventControllerKey *key, guint keyval, guint keycode,
                  GdkModifierType state, gpointer user_data)
{
  (void) key; (void) keycode; (void) state;
  if (keyval == GDK_KEY_Escape) {
    gtk_window_close (GTK_WINDOW (user_data));
    return TRUE;
  }
  return FALSE;
}

/* ── Dialog shell ───────────────────────────────────────────────────── */

GtkWindow *
fw_dialog_new (GtkWindow *parent, const char *title, int w, int h,
               GtkBox **out_content)
{
  GtkWindow *dlg = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_transient_for (dlg, parent);
  gtk_window_set_modal (dlg, TRUE);
  gtk_window_set_destroy_with_parent (dlg, TRUE);
  gtk_window_set_title (dlg, title);
  gtk_window_set_default_size (dlg, w, h);
  gtk_window_set_titlebar (dlg, gtk_header_bar_new ());

  GtkEventController *key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_dialog_escape), dlg);
  gtk_widget_add_controller (GTK_WIDGET (dlg), key);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkBox *content = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 18));
  gtk_widget_set_margin_top    (GTK_WIDGET (content), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (content), 24);
  gtk_widget_set_margin_start  (GTK_WIDGET (content), 24);
  gtk_widget_set_margin_end    (GTK_WIDGET (content), 24);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll),
                                 GTK_WIDGET (content));
  gtk_window_set_child (dlg, scroll);

  *out_content = content;
  return dlg;
}

/* ── Boxed-list rows ────────────────────────────────────────────────── */

static GtkListBox *
fw_boxed_list (void)
{
  GtkListBox *list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (list), "boxed-list");
  return list;
}

GtkListBox *
fw_pref_group (GtkBox *page, const char *title, const char *desc)
{
  GtkBox *group = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 6));
  GtkWidget *tl = gtk_label_new (title);
  gtk_widget_set_halign (tl, GTK_ALIGN_START);
  gtk_widget_add_css_class (tl, "group-title");
  gtk_box_append (group, tl);
  if (desc && *desc) {
    GtkWidget *dl = gtk_label_new (desc);
    gtk_widget_set_halign (dl, GTK_ALIGN_START);
    gtk_label_set_wrap (GTK_LABEL (dl), TRUE);
    gtk_label_set_xalign (GTK_LABEL (dl), 0.0);
    gtk_widget_add_css_class (dl, "dim-label");
    gtk_box_append (group, dl);
  }
  GtkListBox *list = fw_boxed_list ();
  gtk_box_append (group, GTK_WIDGET (list));
  gtk_box_append (page, GTK_WIDGET (group));
  return list;
}

GtkWidget *
fw_value_row (const char *title, const char *subtitle, GtkWidget *suffix)
{
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  GtkBox *hbox = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12));
  gtk_widget_add_css_class (GTK_WIDGET (hbox), "owned-row");
  gtk_widget_set_valign (GTK_WIDGET (hbox), GTK_ALIGN_CENTER);

  GtkBox *text = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 2));
  gtk_widget_set_hexpand (GTK_WIDGET (text), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (text), GTK_ALIGN_CENTER);
  GtkWidget *tl = gtk_label_new (title);
  gtk_widget_set_halign (tl, GTK_ALIGN_START);
  gtk_label_set_ellipsize (GTK_LABEL (tl), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (tl, "owned-row-title");
  gtk_box_append (text, tl);
  if (subtitle && *subtitle) {
    GtkWidget *sl = gtk_label_new (subtitle);
    gtk_widget_set_halign (sl, GTK_ALIGN_START);
    gtk_label_set_wrap (GTK_LABEL (sl), TRUE);
    gtk_label_set_xalign (GTK_LABEL (sl), 0.0);
    gtk_label_set_selectable (GTK_LABEL (sl), TRUE);
    gtk_widget_add_css_class (sl, "owned-row-subtitle");
    gtk_widget_add_css_class (sl, "dim-label");
    gtk_box_append (text, sl);
  }
  gtk_box_append (hbox, GTK_WIDGET (text));

  if (suffix) {
    /* NB: a GtkDropDown suffix triggers a benign GTK log warning about
     * its internal arrow-image's baseline when measured inside the
     * GtkListBox row. It's cosmetic (no layout effect) and unavoidable
     * short of not using GtkDropDown; left as-is. */
    gtk_widget_set_valign (suffix, GTK_ALIGN_CENTER);
    gtk_box_append (hbox, suffix);
  }
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), GTK_WIDGET (hbox));
  return row;
}
