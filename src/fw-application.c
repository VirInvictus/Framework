/* fw-application.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-config.h"
#include "fw-application.h"
#include "fw-window.h"
#include "fw-state.h"
#include "fw-fonts.h"
#include "fw-theme.h"

struct _FwApplication {
  GtkApplication parent_instance;
};

G_DEFINE_FINAL_TYPE (FwApplication, fw_application, GTK_TYPE_APPLICATION)

static void
fw_application_open (GApplication  *app,
                     GFile        **files,
                     int            n_files,
                     const char    *hint)
{
  (void) hint;

  for (int i = 0; i < n_files; i++) {
    FwWindow *win = fw_window_new (FW_APPLICATION (app));
    g_autofree char *path = g_file_get_path (files[i]);
    if (path)
      fw_window_open_file (win, path);
    gtk_window_present (GTK_WINDOW (win));
  }
}

static void
fw_application_activate (GApplication *app)
{
  FwWindow *win = fw_window_new (FW_APPLICATION (app));
  gtk_window_present (GTK_WINDOW (win));
}

static void
open_file_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  FwApplication *self = FW_APPLICATION (user_data);

  g_autoptr (GFile) file = gtk_file_dialog_open_finish (dialog, result, NULL);
  if (!file)
    return;  /* user cancelled */

  g_autofree char *path = g_file_get_path (file);
  if (!path)
    return;

  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (self));
  if (win && FW_IS_WINDOW (win))
    fw_window_open_file (FW_WINDOW (win), path);
}

static void
open_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action;
  (void) parameter;

  FwApplication *self = FW_APPLICATION (user_data);
  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (self));

  if (!win)
    return;

  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Open Document");

  GtkFileFilter *filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, "Supported Documents");
  gtk_file_filter_add_mime_type (filter, "application/pdf");
  gtk_file_filter_add_mime_type (filter, "image/vnd.djvu");
  gtk_file_filter_add_mime_type (filter, "image/x-djvu");
  gtk_file_filter_add_mime_type (filter, "application/vnd.comicbook+zip");
  gtk_file_filter_add_mime_type (filter, "application/vnd.comicbook-rar");
  gtk_file_filter_add_mime_type (filter, "application/x-cbz");
  gtk_file_filter_add_mime_type (filter, "application/x-cbr");
  gtk_file_filter_add_pattern (filter, "*.pdf");
  gtk_file_filter_add_pattern (filter, "*.djvu");
  gtk_file_filter_add_pattern (filter, "*.djv");
  gtk_file_filter_add_pattern (filter, "*.cbz");
  gtk_file_filter_add_pattern (filter, "*.cbr");
  gtk_file_filter_add_pattern (filter, "*.cb7");
  gtk_file_filter_add_pattern (filter, "*.cbt");
  gtk_file_filter_add_pattern (filter, "*.xps");
  gtk_file_filter_add_pattern (filter, "*.oxps");
  gtk_file_filter_add_pattern (filter, "*.epub");
  gtk_file_filter_add_pattern (filter, "*.fb2");
  gtk_file_filter_add_pattern (filter, "*.mobi");
  gtk_file_filter_add_pattern (filter, "*.azw");
  gtk_file_filter_add_pattern (filter, "*.azw3");
  gtk_file_filter_add_pattern (filter, "*.prc");
  gtk_file_filter_add_pattern (filter, "*.txt");
  gtk_file_filter_add_pattern (filter, "*.md");
  gtk_file_filter_add_pattern (filter, "*.markdown");
  gtk_file_filter_add_mime_type (filter, "application/oxps");
  gtk_file_filter_add_mime_type (filter, "application/vnd.ms-xpsdocument");
  gtk_file_filter_add_mime_type (filter, "application/epub+zip");
  gtk_file_filter_add_mime_type (filter, "application/x-fictionbook+xml");
  gtk_file_filter_add_mime_type (filter, "application/x-mobipocket-ebook");
  gtk_file_filter_add_mime_type (filter, "text/plain");
  gtk_file_filter_add_mime_type (filter, "text/markdown");

  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (dialog, filter);

  g_object_unref (filter);
  g_object_unref (filters);

  gtk_file_dialog_open (dialog, win, NULL,
                        (GAsyncReadyCallback) open_file_cb, self);
}

static void
quit_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action;
  (void) parameter;
  /* Close each window rather than calling g_application_quit directly:
   * g_application_quit returns straight from the main loop without
   * emitting close-request, so per-document state (reading position,
   * zoom, rotation) never gets saved.  gtk_window_close fires
   * close-request — where fw_window saves state — and once the last
   * window is gone the application auto-quits (nothing holds it). */
  GtkApplication *app = GTK_APPLICATION (user_data);
  GList *windows = g_list_copy (gtk_application_get_windows (app));
  for (GList *l = windows; l; l = l->next)
    gtk_window_close (GTK_WINDOW (l->data));
  g_list_free (windows);
}

static void
fw_application_startup (GApplication *app)
{
  G_APPLICATION_CLASS (fw_application_parent_class)->startup (app);

  /* Register bundled reading fonts with FontConfig before any view
   * widget tries to resolve a font family. Must happen before window
   * construction so the CSS provider's first load sees them. */
  fw_fonts_register ();

  /* Install the owned stylesheet + portal-driven dark/light theming
   * (the libadwaita replacement, v0.80.0). A display exists here
   * (post chain-up), which the CSS providers need. */
  fw_theme_install ();

  /* Prune stale state entries */
  fw_state_init ();

  /* Actions */
  static const GActionEntry entries[] = {
    { .name = "open", .activate = open_action },
    { .name = "quit", .activate = quit_action },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (app), entries,
                                   G_N_ELEMENTS (entries), app);

  /* Accelerators — registered here because the app object is needed */
  struct { const char *action; const char *accels[4]; } shortcuts[] = {
    { "app.open",            { "<Control>o", NULL } },
    { "app.quit",            { "<Control>q", "<Control>w", NULL } },
    { "win.zoom-in",         { "<Control>plus", "<Control>equal", NULL } },
    { "win.zoom-out",        { "<Control>minus", NULL } },
    { "win.zoom-actual",     { "<Control>0", NULL } },
    { "win.zoom-fit-width",  { "<Control>1", NULL } },
    { "win.zoom-fit-page",   { "<Control>2", NULL } },
    { "win.next-page",       { "Page_Down", NULL } },
    { "win.prev-page",       { "Page_Up", NULL } },
    { "win.first-page",      { "Home", "<Control>Home", NULL } },
    { "win.last-page",       { "End", "<Control>End", NULL } },
    { "win.toggle-sidebar",  { "F9", NULL } },
    { "win.reading-ruler",   { "F8", NULL } },
    { "win.loupe",           { "F7", NULL } },
    { "win.crop-margins",    { "F6", NULL } },
    { "win.manga-mode",      { "F4", NULL } },
    { "win.webtoon-mode",    { "F5", NULL } },
    { "win.facing-pages",    { "F10", NULL } },
    { "win.fullscreen",      { "F11", NULL } },
    { "win.find",            { "<Control>f", NULL } },
    { "win.find-next",       { "F3", NULL } },
    { "win.find-prev",       { "<Shift>F3", NULL } },
    { "win.nav-back",        { "<Alt>Left", NULL } },
    { "win.nav-forward",     { "<Alt>Right", NULL } },
    { "win.go-to-page",      { "<Control>g", NULL } },
    { "win.invert-colors",   { "<Control>i", NULL } },
    { "win.rotate-cw",       { "<Control><Shift>plus", NULL } },
    { "win.rotate-ccw",      { "<Control><Shift>minus", NULL } },
    { "win.copy",            { "<Control>c", NULL } },
    { "win.print",            { "<Control>p", NULL } },
    { "win.show-help-overlay",{ "<Control>question", "F1", NULL } },
  };
  for (size_t i = 0; i < G_N_ELEMENTS (shortcuts); i++)
    gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                            shortcuts[i].action,
                                            shortcuts[i].accels);
}

static void
fw_application_class_init (FwApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);
  app_class->activate = fw_application_activate;
  app_class->open     = fw_application_open;
  app_class->startup  = fw_application_startup;
}

static void
fw_application_init (FwApplication *self)
{
  (void) self;
}

FwApplication *
fw_application_new (void)
{
  return g_object_new (FW_TYPE_APPLICATION,
                       "application-id", APP_ID,
                       "flags", G_APPLICATION_HANDLES_OPEN,
                       NULL);
}
