/* fw-window.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-config.h"
#include "fw-window.h"
#include "fw-view.h"
#include "fw-sidebar.h"
#include "fw-search.h"
#include "fw-cache.h"
#include "fw-document.h"
#include "fw-state.h"
#include "fw-debug.h"
#include <gdk/gdk.h>

struct _FwWindow {
  AdwApplicationWindow  parent_instance;

  /* Document state */
  FwDocument           *document;
  FwCache              *cache;

  /* Header bar controls */
  AdwHeaderBar         *header_bar;
  GtkButton            *sidebar_button;
  GtkButton            *zoom_out_button;
  GtkEntry             *zoom_entry;
  GtkButton            *zoom_in_button;
  GtkLabel             *title_label;
  GtkEntry             *page_entry;
  GtkButton            *prev_page_button;
  GtkButton            *next_page_button;
  GtkToggleButton      *search_toggle;

  /* Layout */
  AdwOverlaySplitView  *split_view;
  FwSidebar            *sidebar;
  GtkScrolledWindow    *scroll;
  FwView               *view;
  FwSearch             *search;
  GtkSearchBar         *search_bar;

  /* State */
  double                zoom;
  int                   rotation;
  int                   current_page;
  gboolean              invert_colors;
  char                 *file_path;   /* absolute path of current document */

  /* Deferred restore */
  double                _restore_scroll;  /* scroll fraction to restore */
  gboolean              _restore_pending;
};

G_DEFINE_FINAL_TYPE (FwWindow, fw_window, ADW_TYPE_APPLICATION_WINDOW)

static void fw_window_save_state (FwWindow *self);

/* ── Zoom ─────────────────────────────────────────────────────────── */

static void
update_zoom_entry (FwWindow *self)
{
  char buf[16];
  g_snprintf (buf, sizeof buf, "%.0f%%", self->zoom * 100.0);
  GtkEntryBuffer *buffer = gtk_entry_get_buffer (self->zoom_entry);
  gtk_entry_buffer_set_text (buffer, buf, -1);
}

static void
set_zoom (FwWindow *self, double zoom)
{
  if (zoom < 0.1) zoom = 0.1;
  if (zoom > 10.0) zoom = 10.0;
  FW_TRACE_WINDOW ("set_zoom: %.2f", zoom);
  self->zoom = zoom;
  update_zoom_entry (self);

  if (self->cache)
    fw_cache_start (self->cache, self->zoom, self->rotation);

  if (self->view)
    fw_view_set_zoom (self->view, zoom);
}

static void
zoom_in_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FwWindow *self = FW_WINDOW (user_data);
  set_zoom (self, self->zoom + 0.1);
}

static void
zoom_out_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FwWindow *self = FW_WINDOW (user_data);
  set_zoom (self, self->zoom - 0.1);
}

static void
zoom_entry_activated (GtkEntry *entry, gpointer user_data)
{
  FwWindow *self = FW_WINDOW (user_data);
  const char *text = gtk_entry_buffer_get_text (
    gtk_entry_get_buffer (entry));
  double val = g_ascii_strtod (text, NULL);
  if (val > 0)
    set_zoom (self, val / 100.0);
  else
    update_zoom_entry (self);
}

/* ── Navigation ───────────────────────────────────────────────────── */

static void
update_page_entry (FwWindow *self)
{
  if (!self->document)
    return;
  int total = fw_document_get_page_count (self->document);
  char buf[32];
  g_snprintf (buf, sizeof buf, "%d / %d", self->current_page + 1, total);
  GtkEntryBuffer *buffer = gtk_entry_get_buffer (self->page_entry);
  gtk_entry_buffer_set_text (buffer, buf, -1);
}

static void
go_to_page (FwWindow *self, int page)
{
  if (!self->document)
    return;
  int total = fw_document_get_page_count (self->document);
  if (page < 0) page = 0;
  if (page >= total) page = total - 1;
  self->current_page = page;
  update_page_entry (self);

  if (self->view)
    fw_view_go_to_page (self->view, page);
}

static void
prev_page_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FwWindow *self = FW_WINDOW (user_data);
  go_to_page (self, self->current_page - 1);
}

static void
next_page_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FwWindow *self = FW_WINDOW (user_data);
  go_to_page (self, self->current_page + 1);
}

static void
page_entry_activated (GtkEntry *entry, gpointer user_data)
{
  FwWindow *self = FW_WINDOW (user_data);
  const char *text = gtk_entry_buffer_get_text (
    gtk_entry_get_buffer (entry));
  int page = (int) g_ascii_strtoll (text, NULL, 10) - 1;
  go_to_page (self, page);
}

/* ── Scroll tracking ─────────────────────────────────────────────── */

static void
on_scroll_changed (GtkAdjustment *adj, gpointer user_data)
{
  (void) adj;
  FwWindow *self = FW_WINDOW (user_data);
  if (!self->view || !self->document)
    return;

  int page = fw_view_get_current_page (self->view);
  if (page != self->current_page) {
    self->current_page = page;
    update_page_entry (self);
  }
}

/* ── Search toggle ────────────────────────────────────────────────── */

static void
search_toggled (GtkToggleButton *button, gpointer user_data)
{
  FwWindow *self = FW_WINDOW (user_data);
  gboolean active = gtk_toggle_button_get_active (button);
  gtk_search_bar_set_search_mode (self->search_bar, active);
}

/* ── Sidebar toggle ───────────────────────────────────────────────── */

static void
sidebar_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FwWindow *self = FW_WINDOW (user_data);
  gboolean visible = adw_overlay_split_view_get_show_sidebar (self->split_view);
  adw_overlay_split_view_set_show_sidebar (self->split_view, !visible);
}

/* ── Sidebar TOC navigation ───────────────────────────────────────── */

static void
on_sidebar_page_requested (FwSidebar *sidebar, int page, gpointer user_data)
{
  (void) sidebar;
  FwWindow *self = FW_WINDOW (user_data);
  if (self->document && page >= 0)
    go_to_page (self, page);
}

/* ── Window actions ───────────────────────────────────────────────── */

static void act_zoom_in    (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; set_zoom(w, w->zoom + 0.1); }
static void act_zoom_out   (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; set_zoom(w, w->zoom - 0.1); }
static void act_zoom_actual(GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; set_zoom(d, 1.0); }
static void act_zoom_fit_w (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; set_zoom(w, fw_view_fit_width_zoom(w->view, gtk_widget_get_width(GTK_WIDGET(w->scroll)))); }
static void act_zoom_fit_p (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; (void)d; /* TODO: fit-page */ }
static void act_next_page  (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; go_to_page(w, w->current_page + 1); }
static void act_prev_page  (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; go_to_page(w, w->current_page - 1); }
static void act_first_page (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; go_to_page(d, 0); }
static void act_last_page  (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; if(w->document) go_to_page(w, fw_document_get_page_count(w->document)-1); }
static void act_toggle_sidebar (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; sidebar_clicked(NULL, w); }

static void act_toggle_fullscreen (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  if (gtk_window_is_fullscreen (GTK_WINDOW (w)))
    gtk_window_unfullscreen (GTK_WINDOW (w));
  else
    gtk_window_fullscreen (GTK_WINDOW (w));
}

static void act_find (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  gtk_toggle_button_set_active (w->search_toggle, TRUE);
}

static void act_go_to_page (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  gtk_widget_grab_focus (GTK_WIDGET (w->page_entry));
  gtk_editable_select_region (GTK_EDITABLE (w->page_entry), 0, -1);
}

static void act_invert_colors (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  w->invert_colors = !w->invert_colors;
  if (w->view)
    fw_view_set_invert (w->view, w->invert_colors);
}

static void act_print (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  (void)d;
  /* TODO: implement printing via GtkPrintOperation */
}

static void act_about (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  AdwAboutDialog *dlg = ADW_ABOUT_DIALOG (adw_about_dialog_new ());
  adw_about_dialog_set_application_name (dlg, "Framework");
  adw_about_dialog_set_version (dlg, APP_VERSION);
  adw_about_dialog_set_comments (dlg,
    "A fast, native GNOME document viewer built on MuPDF and DjVuLibre.");
  adw_about_dialog_set_application_icon (dlg, APP_ID);
  adw_about_dialog_set_license_type (dlg, GTK_LICENSE_GPL_3_0);
  adw_about_dialog_set_website (dlg, "https://github.com/VirInvictus/Framework");
  adw_about_dialog_set_issue_url (dlg,
    "https://github.com/VirInvictus/Framework/issues");
  const char *developers[] = { "Brandon LaRocque", NULL };
  adw_about_dialog_set_developers (dlg, developers);
  adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (w));
}

/* ── Arrow key scrolling & Ctrl+Scroll zoom ─────────────────────── */

#define SCROLL_STEP 60

static gboolean
on_scroll (GtkEventControllerScroll *controller,
           double                    dx,
           double                    dy,
           gpointer                  user_data)
{
  (void) controller; (void) dx;
  FwWindow *self = FW_WINDOW (user_data);

  if (!self->view || dy == 0)
    return FALSE;

  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (controller));
  if ((state & GDK_CONTROL_MASK) != 0) {
    if (dy < 0) {
      set_zoom (self, self->zoom + 0.1);
    } else if (dy > 0) {
      double new_zoom = self->zoom - 0.1;
      if (new_zoom < 0.1) new_zoom = 0.1;
      set_zoom (self, new_zoom);
    }
    return TRUE;
  }
  
  return FALSE;
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                gpointer               user_data)
{
  (void) controller; (void) keycode; (void) state;
  FwWindow *self = FW_WINDOW (user_data);

  if (!self->view)
    return FALSE;

  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scroll);
  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (self->scroll);

  switch (keyval) {
  case GDK_KEY_Up:
    if (vadj) {
      double v = gtk_adjustment_get_value (vadj);
      gtk_adjustment_set_value (vadj, v - SCROLL_STEP);
    }
    return TRUE;
  case GDK_KEY_Down:
    if (vadj) {
      double v = gtk_adjustment_get_value (vadj);
      gtk_adjustment_set_value (vadj, v + SCROLL_STEP);
    }
    return TRUE;
  case GDK_KEY_Left:
    if (hadj) {
      double v = gtk_adjustment_get_value (hadj);
      gtk_adjustment_set_value (hadj, v - SCROLL_STEP);
    }
    return TRUE;
  case GDK_KEY_Right:
    if (hadj) {
      double v = gtk_adjustment_get_value (hadj);
      gtk_adjustment_set_value (hadj, v + SCROLL_STEP);
    }
    return TRUE;
  default:
    return FALSE;
  }
}

/* ── Scale factor change ─────────────────────────────────────────── */

static void
on_scale_factor_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  FwWindow *self = FW_WINDOW (object);
  (void) user_data;

  if (!self->cache)
    return;

  int sf = gtk_widget_get_scale_factor (GTK_WIDGET (self));
  fw_cache_set_scale_factor (self->cache, sf);
  fw_cache_start (self->cache, self->zoom, self->rotation);
}

/* ── Close handler ───────────────────────────────────────────────── */

static gboolean
on_close_request (GtkWindow *window, gpointer user_data)
{
  (void) user_data;
  fw_window_save_state (FW_WINDOW (window));
  return FALSE;  /* allow the window to close */
}

/* ── Build UI ─────────────────────────────────────────────────────── */

static void
fw_window_constructed (GObject *object)
{
  FwWindow *self = FW_WINDOW (object);
  G_OBJECT_CLASS (fw_window_parent_class)->constructed (object);

  gtk_window_set_default_size (GTK_WINDOW (self), 900, 700);
  gtk_window_set_title (GTK_WINDOW (self), "Framework");

  /* ── Header bar ── */
  self->header_bar = ADW_HEADER_BAR (adw_header_bar_new ());

  /* Left: sidebar toggle */
  self->sidebar_button = GTK_BUTTON (gtk_button_new_from_icon_name (
    "sidebar-show-symbolic"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->sidebar_button),
                               "Toggle Sidebar (F9)");
  g_signal_connect (self->sidebar_button, "clicked",
                    G_CALLBACK (sidebar_clicked), self);
  adw_header_bar_pack_start (self->header_bar,
                              GTK_WIDGET (self->sidebar_button));

  /* Left: zoom controls */
  self->zoom_out_button = GTK_BUTTON (gtk_button_new_from_icon_name (
    "zoom-out-symbolic"));
  g_signal_connect (self->zoom_out_button, "clicked",
                    G_CALLBACK (zoom_out_clicked), self);
  adw_header_bar_pack_start (self->header_bar,
                              GTK_WIDGET (self->zoom_out_button));

  self->zoom_entry = GTK_ENTRY (gtk_entry_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->zoom_entry), 70, -1);
  gtk_entry_set_alignment (self->zoom_entry, 0.5);
  g_signal_connect (self->zoom_entry, "activate",
                    G_CALLBACK (zoom_entry_activated), self);
  adw_header_bar_pack_start (self->header_bar,
                              GTK_WIDGET (self->zoom_entry));

  self->zoom_in_button = GTK_BUTTON (gtk_button_new_from_icon_name (
    "zoom-in-symbolic"));
  g_signal_connect (self->zoom_in_button, "clicked",
                    G_CALLBACK (zoom_in_clicked), self);
  adw_header_bar_pack_start (self->header_bar,
                              GTK_WIDGET (self->zoom_in_button));

  /* Title */
  self->title_label = GTK_LABEL (gtk_label_new ("Framework"));
  gtk_label_set_ellipsize (self->title_label, PANGO_ELLIPSIZE_END);
  adw_header_bar_set_title_widget (self->header_bar,
                                    GTK_WIDGET (self->title_label));

  /* Right: primary menu */
  GMenu *menu = g_menu_new ();
  g_menu_append (menu, "Open...", "app.open");

  GMenu *zoom_section = g_menu_new ();
  g_menu_append (zoom_section, "Fit Width", "win.zoom-fit-width");
  g_menu_append (zoom_section, "Fit Page", "win.zoom-fit-page");
  g_menu_append (zoom_section, "Actual Size (100%)", "win.zoom-actual");
  g_menu_append_submenu (menu, "Zoom", G_MENU_MODEL (zoom_section));

  g_menu_append (menu, "Invert Colors", "win.invert-colors");
  g_menu_append (menu, "Print...", "win.print");
  g_menu_append (menu, "About Framework", "win.about");

  GtkMenuButton *menu_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  gtk_menu_button_set_icon_name (menu_button, "open-menu-symbolic");
  gtk_menu_button_set_menu_model (menu_button, G_MENU_MODEL (menu));
  adw_header_bar_pack_end (self->header_bar, GTK_WIDGET (menu_button));
  g_object_unref (menu);
  g_object_unref (zoom_section);

  /* Right: search toggle */
  self->search_toggle = GTK_TOGGLE_BUTTON (gtk_toggle_button_new ());
  gtk_button_set_icon_name (GTK_BUTTON (self->search_toggle),
                            "edit-find-symbolic");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->search_toggle),
                               "Find (Ctrl+F)");
  g_signal_connect (self->search_toggle, "toggled",
                    G_CALLBACK (search_toggled), self);
  adw_header_bar_pack_end (self->header_bar,
                            GTK_WIDGET (self->search_toggle));

  /* Right: page navigation */
  self->next_page_button = GTK_BUTTON (gtk_button_new_from_icon_name (
    "go-down-symbolic"));
  g_signal_connect (self->next_page_button, "clicked",
                    G_CALLBACK (next_page_clicked), self);
  adw_header_bar_pack_end (self->header_bar,
                            GTK_WIDGET (self->next_page_button));

  self->prev_page_button = GTK_BUTTON (gtk_button_new_from_icon_name (
    "go-up-symbolic"));
  g_signal_connect (self->prev_page_button, "clicked",
                    G_CALLBACK (prev_page_clicked), self);
  adw_header_bar_pack_end (self->header_bar,
                            GTK_WIDGET (self->prev_page_button));

  self->page_entry = GTK_ENTRY (gtk_entry_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->page_entry), 90, -1);
  gtk_entry_set_alignment (self->page_entry, 0.5);
  g_signal_connect (self->page_entry, "activate",
                    G_CALLBACK (page_entry_activated), self);
  adw_header_bar_pack_end (self->header_bar,
                            GTK_WIDGET (self->page_entry));

  /* ── Content area ── */
  self->view = fw_view_new ();
  self->scroll = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_child (self->scroll, GTK_WIDGET (self->view));
  gtk_scrolled_window_set_policy (self->scroll,
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_kinetic_scrolling (self->scroll, TRUE);

  /* Track current page as user scrolls */
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scroll);
  if (vadj)
    g_signal_connect (vadj, "value-changed",
                      G_CALLBACK (on_scroll_changed), self);
  gtk_widget_set_vexpand (GTK_WIDGET (self->scroll), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->scroll), TRUE);

  /* Search bar overlay */
  GtkSearchEntry *search_entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  self->search_bar = GTK_SEARCH_BAR (gtk_search_bar_new ());
  gtk_search_bar_set_child (self->search_bar, GTK_WIDGET (search_entry));
  gtk_search_bar_connect_entry (self->search_bar, GTK_EDITABLE (search_entry));
  gtk_widget_set_valign (GTK_WIDGET (self->search_bar), GTK_ALIGN_START);

  GtkOverlay *overlay = GTK_OVERLAY (gtk_overlay_new ());
  gtk_overlay_set_child (overlay, GTK_WIDGET (self->scroll));
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self->search_bar));
  gtk_widget_set_vexpand (GTK_WIDGET (overlay), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (overlay), TRUE);

  /* Sidebar */
  self->sidebar = fw_sidebar_new ();
  g_signal_connect (self->sidebar, "page-requested",
                    G_CALLBACK (on_sidebar_page_requested), self);
  GtkScrolledWindow *sidebar_scroll = GTK_SCROLLED_WINDOW (
    gtk_scrolled_window_new ());
  gtk_scrolled_window_set_child (sidebar_scroll,
                                  GTK_WIDGET (self->sidebar));

  /* Split view */
  self->split_view = ADW_OVERLAY_SPLIT_VIEW (adw_overlay_split_view_new ());
  adw_overlay_split_view_set_sidebar (self->split_view,
                                       GTK_WIDGET (sidebar_scroll));
  adw_overlay_split_view_set_content (self->split_view,
                                       GTK_WIDGET (overlay));
  adw_overlay_split_view_set_show_sidebar (self->split_view, FALSE);
  adw_overlay_split_view_set_max_sidebar_width (self->split_view, 280);
  gtk_widget_set_vexpand (GTK_WIDGET (self->split_view), TRUE);

  /* ── Main box ── */
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_append (box, GTK_WIDGET (self->header_bar));
  gtk_box_append (box, GTK_WIDGET (self->split_view));

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                       GTK_WIDGET (box));

  /* Initialize state */
  self->zoom = 1.0;
  self->rotation = 0;
  self->current_page = 0;
  update_zoom_entry (self);

  /* ── Window actions ── */
  static const GActionEntry win_entries[] = {
    { .name = "zoom-in",       .activate = act_zoom_in },
    { .name = "zoom-out",      .activate = act_zoom_out },
    { .name = "zoom-actual",   .activate = act_zoom_actual },
    { .name = "zoom-fit-width",.activate = act_zoom_fit_w },
    { .name = "zoom-fit-page", .activate = act_zoom_fit_p },
    { .name = "next-page",     .activate = act_next_page },
    { .name = "prev-page",     .activate = act_prev_page },
    { .name = "first-page",    .activate = act_first_page },
    { .name = "last-page",     .activate = act_last_page },
    { .name = "toggle-sidebar",.activate = act_toggle_sidebar },
    { .name = "fullscreen",    .activate = act_toggle_fullscreen },
    { .name = "find",          .activate = act_find },
    { .name = "go-to-page",    .activate = act_go_to_page },
    { .name = "invert-colors", .activate = act_invert_colors },
    { .name = "print",         .activate = act_print },
    { .name = "about",         .activate = act_about },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (self), win_entries,
                                   G_N_ELEMENTS (win_entries), self);

  /* ── Arrow key scrolling & Ctrl+Scroll zoom ── */
  GtkEventController *key_ctl =
    gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (key_ctl, GTK_PHASE_CAPTURE);
  g_signal_connect (key_ctl, "key-pressed",
                    G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key_ctl);

  GtkEventController *scroll_ctl =
    gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  gtk_event_controller_set_propagation_phase (scroll_ctl, GTK_PHASE_CAPTURE);
  g_signal_connect (scroll_ctl, "scroll",
                    G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll_ctl);

  /* Make view focusable and give it initial focus so arrow keys
   * don't land in the header bar entries. */
  gtk_widget_set_focusable (GTK_WIDGET (self->view), TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));

  /* Save document state before the window is destroyed */
  g_signal_connect (self, "close-request",
                    G_CALLBACK (on_close_request), self);

  /* Update render resolution when moving between monitors with different
   * scale factors (e.g., 1x laptop → 2x external display) */
  g_signal_connect (self, "notify::scale-factor",
                    G_CALLBACK (on_scale_factor_changed), self);
}

/* ── Deferred fit-width via tick callback ─────────────────────────── */

static gboolean
apply_fit_width_tick (GtkWidget *widget, GdkFrameClock *clock,
                      gpointer user_data)
{
  (void) clock;
  FwWindow *self = FW_WINDOW (user_data);

  int vw = gtk_widget_get_width (GTK_WIDGET (self->scroll));
  if (vw <= 0)
    return G_SOURCE_CONTINUE;  /* not allocated yet, try next frame */

  double fit_zoom = fw_view_fit_width_zoom (self->view, vw);
  set_zoom (self, fit_zoom);

  /* Remove ourselves — return FALSE stops the tick callback */
  (void) widget;
  return G_SOURCE_REMOVE;
}

/* ── Deferred state restore via tick callback ────────────────────── */

static gboolean
restore_state_tick (GtkWidget *widget, GdkFrameClock *clock,
                    gpointer user_data)
{
  (void) clock; (void) widget;
  FwWindow *self = FW_WINDOW (user_data);

  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scroll);
  if (!vadj)
    return G_SOURCE_CONTINUE;

  double upper = gtk_adjustment_get_upper (vadj);
  if (upper <= 0)
    return G_SOURCE_CONTINUE;  /* layout not ready yet */

  /* Restore: go to saved page, then fine-tune with scroll fraction */
  if (self->view)
    fw_view_go_to_page (self->view, self->current_page);

  if (self->_restore_scroll > 0) {
    /* Re-read upper after go_to_page may have triggered layout */
    upper = gtk_adjustment_get_upper (vadj);
    gtk_adjustment_set_value (vadj, self->_restore_scroll * upper);
  }

  update_page_entry (self);
  self->_restore_pending = FALSE;
  return G_SOURCE_REMOVE;
}

/* ── Open file ────────────────────────────────────────────────────── */

void
fw_window_open_file (FwWindow *self, const char *path)
{
  g_return_if_fail (FW_IS_WINDOW (self));
  g_return_if_fail (path != NULL);

  FW_TRACE_WINDOW ("open_file: '%s'", path);

  /* Save state of previous document before switching */
  fw_window_save_state (self);

  /* Clean up previous document */
  if (self->cache) {
    FW_TRACE_MEM ("closing previous doc: cache=%p doc=%p",
                  (void *) self->cache, (void *) self->document);
    fw_cache_stop (self->cache);
    g_clear_object (&self->cache);
  }
  g_clear_object (&self->document);
  g_clear_pointer (&self->file_path, g_free);

  /* Open new document */
  g_autoptr (GError) error = NULL;
  self->document = fw_document_new_for_path (path, &error);

  if (!self->document) {
    AdwAlertDialog *dlg = ADW_ALERT_DIALOG (
      adw_alert_dialog_new ("Cannot Open Document", error->message));
    adw_alert_dialog_add_response (dlg, "ok", "OK");
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
    return;
  }

  self->file_path = g_strdup (path);

  /* Update title */
  g_autofree char *basename = g_path_get_basename (path);
  gtk_label_set_text (self->title_label, basename);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->title_label), path);

  /* Load saved state for this document */
  FwDocumentState *saved = fw_state_load (path);

  /* Configure view */
  self->cache = fw_cache_new (self->document, GTK_WIDGET (self->view));
  fw_cache_set_scale_factor (self->cache,
    gtk_widget_get_scale_factor (GTK_WIDGET (self)));
  fw_view_set_document (self->view, self->document, self->cache);

  /* Apply saved zoom or default to fit-width.
   * set_zoom() already calls fw_cache_start() internally — do NOT call
   * fw_cache_start() again here.  A redundant call bumps the generation
   * counter, making the workers that set_zoom() just submitted stale.
   * For DjVu (serialized rendering), this means the first visible pages
   * sit in the "stale" queue and never produce surfaces until the user
   * scrolls, which is the "pages don't render on open" bug. */
  if (saved) {
    self->rotation = saved->rotation;
    set_zoom (self, saved->zoom_level);
    self->current_page = saved->page;
  } else {
    set_zoom (self, 1.0);
    self->current_page = 0;
  }

  update_page_entry (self);

  /* Defer scroll position restore (and fit-width for new docs) until
   * the scrolled window has a real allocation. */
  if (saved) {
    /* Store the scroll fraction to restore after layout */
    self->_restore_scroll = saved->scroll_position;
    self->_restore_pending = TRUE;
    gtk_widget_add_tick_callback (GTK_WIDGET (self->scroll),
                                  restore_state_tick, self, NULL);
    fw_document_state_free (saved);
  } else {
    self->_restore_pending = FALSE;
    gtk_widget_add_tick_callback (GTK_WIDGET (self->scroll),
                                  apply_fit_width_tick, self, NULL);
  }

  FW_TRACE_WINDOW ("open_file done: zoom=%.2f page=%d restore=%s",
                    self->zoom, self->current_page,
                    self->_restore_pending ? "yes" : "no");

  /* Load TOC into sidebar */
  FwTocNode *toc = fw_document_get_toc (self->document);
  fw_sidebar_set_toc (self->sidebar, toc);
  fw_toc_node_free (toc);
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_window_save_state (FwWindow *self)
{
  if (!self->file_path || !self->document)
    return;

  /* Get current page and scroll position from the view */
  int page = self->view ? fw_view_get_current_page (self->view) : 0;

  double scroll_frac = 0;
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scroll);
  if (vadj) {
    double upper = gtk_adjustment_get_upper (vadj);
    if (upper > 0)
      scroll_frac = gtk_adjustment_get_value (vadj) / upper;
  }

  FwDocumentState state = {
    .page            = page,
    .scroll_position = scroll_frac,
    .zoom_level      = self->zoom,
    .zoom_mode       = (char *) "fit-width",
    .view_mode       = (char *) "continuous",
    .rotation        = self->rotation,
  };

  fw_state_save (self->file_path, &state);
}

static void
fw_window_dispose (GObject *object)
{
  FwWindow *self = FW_WINDOW (object);

  fw_window_save_state (self);

  /* Disconnect view from document/cache FIRST — the view holds refs to both,
   * and GTK widget teardown order is unpredictable. Without this, the cache
   * refcount never reaches zero during window dispose. */
  if (self->view)
    fw_view_set_document (self->view, NULL, NULL);

  if (self->cache) {
    FW_TRACE_MEM ("window dispose: stopping cache=%p", (void *) self->cache);
    fw_cache_stop (self->cache);
    g_clear_object (&self->cache);
  }
  FW_TRACE_MEM ("window dispose: clearing doc=%p path='%s'",
                (void *) self->document,
                self->file_path ? self->file_path : "(null)");
  g_clear_object (&self->document);
  g_clear_pointer (&self->file_path, g_free);

  G_OBJECT_CLASS (fw_window_parent_class)->dispose (object);
}

static void
fw_window_class_init (FwWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->constructed = fw_window_constructed;
  object_class->dispose     = fw_window_dispose;
}

static void
fw_window_init (FwWindow *self)
{
  (void) self;
}

FwWindow *
fw_window_new (FwApplication *app)
{
  return g_object_new (FW_TYPE_WINDOW,
                       "application", app,
                       NULL);
}
