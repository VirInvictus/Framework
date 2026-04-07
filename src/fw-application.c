/* fw-application.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-config.h"
#include "fw-application.h"
#include "fw-window.h"

struct _FwApplication {
  AdwApplication parent_instance;
};

G_DEFINE_FINAL_TYPE (FwApplication, fw_application, ADW_TYPE_APPLICATION)

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
  gtk_file_filter_add_pattern (filter, "*.pdf");
  gtk_file_filter_add_pattern (filter, "*.djvu");
  gtk_file_filter_add_pattern (filter, "*.djv");

  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (dialog, filter);

  g_object_unref (filter);
  g_object_unref (filters);

  gtk_file_dialog_open (dialog, win, NULL, NULL, NULL);
  /* TODO: connect async callback to open the selected file */
  g_object_unref (dialog);
}

static void
quit_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action;
  (void) parameter;
  g_application_quit (G_APPLICATION (user_data));
}

static void
fw_application_startup (GApplication *app)
{
  G_APPLICATION_CLASS (fw_application_parent_class)->startup (app);

  /* Actions */
  static const GActionEntry entries[] = {
    { .name = "open", .activate = open_action },
    { .name = "quit", .activate = quit_action },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (app), entries,
                                   G_N_ELEMENTS (entries), app);

  /* Accelerators */
  const char *open_accels[]  = { "<Control>o", NULL };
  const char *quit_accels[]  = { "<Control>q", "<Control>w", NULL };

  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                          "app.open", open_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                          "app.quit", quit_accels);
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
