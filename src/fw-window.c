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
};

G_DEFINE_FINAL_TYPE (FwWindow, fw_window, ADW_TYPE_APPLICATION_WINDOW)

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
    "view-sidebar-symbolic"));
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
  g_menu_append (menu, "Keyboard Shortcuts", "win.show-help-overlay");
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
  };
  g_action_map_add_action_entries (G_ACTION_MAP (self), win_entries,
                                   G_N_ELEMENTS (win_entries), self);

  /* ── Keyboard accelerators ── */
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  if (app) {
    struct { const char *action; const char *accels[4]; } shortcuts[] = {
      { "win.zoom-in",       { "<Control>plus", "<Control>equal", NULL } },
      { "win.zoom-out",      { "<Control>minus", NULL } },
      { "win.zoom-actual",   { "<Control>0", NULL } },
      { "win.zoom-fit-width",{ "<Control>1", NULL } },
      { "win.zoom-fit-page", { "<Control>2", NULL } },
      { "win.next-page",     { "Page_Down", NULL } },
      { "win.prev-page",     { "Page_Up", NULL } },
      { "win.first-page",    { "Home", "<Control>Home", NULL } },
      { "win.last-page",     { "End", "<Control>End", NULL } },
      { "win.toggle-sidebar",{ "F9", NULL } },
      { "win.fullscreen",    { "F11", NULL } },
      { "win.find",          { "<Control>f", NULL } },
      { "win.go-to-page",    { "<Control>g", NULL } },
    };
    for (size_t i = 0; i < G_N_ELEMENTS (shortcuts); i++)
      gtk_application_set_accels_for_action (app, shortcuts[i].action,
                                              shortcuts[i].accels);
  }
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

/* ── Open file ────────────────────────────────────────────────────── */

void
fw_window_open_file (FwWindow *self, const char *path)
{
  g_return_if_fail (FW_IS_WINDOW (self));
  g_return_if_fail (path != NULL);

  /* Clean up previous document */
  if (self->cache) {
    fw_cache_stop (self->cache);
    g_clear_object (&self->cache);
  }
  g_clear_object (&self->document);

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

  /* Update title */
  g_autofree char *basename = g_path_get_basename (path);
  gtk_label_set_text (self->title_label, basename);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->title_label), path);

  /* Configure view first (so it has the document for fit-width calc) */
  self->cache = fw_cache_new (self->document, GTK_WIDGET (self->view));
  fw_view_set_document (self->view, self->document, self->cache);

  /* Defer fit-width until the scrolled window has a real allocation */
  set_zoom (self, 1.0);
  fw_cache_start (self->cache, self->zoom, self->rotation);
  gtk_widget_add_tick_callback (GTK_WIDGET (self->scroll),
                                apply_fit_width_tick, self, NULL);

  /* Update page entry */
  self->current_page = 0;
  update_page_entry (self);

  /* Load TOC into sidebar */
  FwTocNode *toc = fw_document_get_toc (self->document);
  fw_sidebar_set_toc (self->sidebar, toc);
  fw_toc_node_free (toc);
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_window_dispose (GObject *object)
{
  FwWindow *self = FW_WINDOW (object);

  if (self->cache) {
    fw_cache_stop (self->cache);
    g_clear_object (&self->cache);
  }
  g_clear_object (&self->document);

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
