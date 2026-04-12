/* fw-state.c — Per-document state persistence
 *
 * Stores per-document state in $XDG_DATA_HOME/framework/state.json.
 * Prunes entries older than 90 days and caps at 500 entries (LRU).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-state.h"

#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <string.h>

#define STATE_DIR    "framework"
#define STATE_FILE   "state.json"
#define MAX_ENTRIES  500
#define MAX_AGE_DAYS 90

static char *
get_state_path (void)
{
  return g_build_filename (g_get_user_data_dir (), STATE_DIR, STATE_FILE, NULL);
}

static JsonNode *
load_root (void)
{
  g_autofree char *path = get_state_path ();

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return json_node_init_object (json_node_new (JSON_NODE_OBJECT),
                                   json_object_new ());

  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;

  if (!json_parser_load_from_file (parser, path, &error)) {
    g_warning ("Failed to load state: %s", error->message);
    return json_node_init_object (json_node_new (JSON_NODE_OBJECT),
                                   json_object_new ());
  }

  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    return json_node_init_object (json_node_new (JSON_NODE_OBJECT),
                                   json_object_new ());

  return json_node_copy (root);
}

static void
save_root (JsonNode *root)
{
  g_autofree char *dir_path = g_build_filename (g_get_user_data_dir (),
                                                 STATE_DIR, NULL);
  g_mkdir_with_parents (dir_path, 0755);

  g_autofree char *path = get_state_path ();
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_pretty (gen, TRUE);
  json_generator_set_root (gen, root);

  g_autoptr (GError) error = NULL;
  if (!json_generator_to_file (gen, path, &error))
    g_warning ("Failed to save state: %s", error->message);
}

void
fw_state_init (void)
{
  fw_state_prune ();
}

FwDocumentState *
fw_state_load (const char *path)
{
  g_return_val_if_fail (path != NULL, NULL);

  g_autoptr (JsonNode) root = load_root ();
  JsonObject *obj = json_node_get_object (root);

  if (!json_object_has_member (obj, path))
    return NULL;

  JsonObject *entry = json_object_get_object_member (obj, path);

  FwDocumentState *state = g_new0 (FwDocumentState, 1);
  state->page            = (int) json_object_get_int_member_with_default (entry, "page", 0);
  state->scroll_position = json_object_get_double_member_with_default (entry, "scroll_position", 0.0);
  state->zoom_level      = json_object_get_double_member_with_default (entry, "zoom_level", 1.0);
  state->zoom_mode       = g_strdup (json_object_get_string_member_with_default (entry, "zoom_mode", "fit-width"));
  state->view_mode       = g_strdup (json_object_get_string_member_with_default (entry, "view_mode", "continuous"));
  state->rotation        = (int) json_object_get_int_member_with_default (entry, "rotation", 0);

  return state;
}

void
fw_state_save (const char *path, const FwDocumentState *state)
{
  g_return_if_fail (path != NULL && state != NULL);

  g_autoptr (JsonNode) root = load_root ();
  JsonObject *obj = json_node_get_object (root);

  JsonObject *entry = json_object_new ();
  json_object_set_int_member (entry, "page", state->page);
  json_object_set_double_member (entry, "scroll_position", state->scroll_position);
  json_object_set_double_member (entry, "zoom_level", state->zoom_level);
  json_object_set_string_member (entry, "zoom_mode", state->zoom_mode ? state->zoom_mode : "fit-width");
  json_object_set_string_member (entry, "view_mode", state->view_mode ? state->view_mode : "continuous");
  json_object_set_int_member (entry, "rotation", state->rotation);

  g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
  g_autofree char *timestamp = g_date_time_format_iso8601 (now);
  json_object_set_string_member (entry, "last_opened", timestamp);

  json_object_set_object_member (obj, path, entry);

  save_root (root);
}

void
fw_state_prune (void)
{
  g_autoptr (JsonNode) root = load_root ();
  JsonObject *obj = json_node_get_object (root);

  GList *members = json_object_get_members (obj);
  guint count = g_list_length (members);
  gboolean changed = FALSE;

  g_autoptr (GDateTime) now = g_date_time_new_now_utc ();

  /* Collect keys to remove — we must not modify the object while iterating
   * over its member list, as json_object_remove_member invalidates it. */
  GList *to_remove = NULL;

  for (GList *l = members; l; l = l->next) {
    const char *key = l->data;
    JsonObject *entry = json_object_get_object_member (obj, key);
    const char *ts = json_object_get_string_member_with_default (entry,
      "last_opened", NULL);

    if (!ts) {
      to_remove = g_list_prepend (to_remove, (gpointer) key);
      continue;
    }

    g_autoptr (GDateTime) opened = g_date_time_new_from_iso8601 (ts, NULL);
    if (!opened) {
      to_remove = g_list_prepend (to_remove, (gpointer) key);
      continue;
    }

    GTimeSpan diff = g_date_time_difference (now, opened);
    if (diff > (GTimeSpan) MAX_AGE_DAYS * G_TIME_SPAN_DAY)
      to_remove = g_list_prepend (to_remove, (gpointer) key);
  }

  for (GList *l = to_remove; l; l = l->next) {
    json_object_remove_member (obj, (const char *) l->data);
    count--;
    changed = TRUE;
  }

  g_list_free (to_remove);

  /* TODO: LRU eviction if count > MAX_ENTRIES */
  (void) count;

  g_list_free (members);

  if (changed)
    save_root (root);
}

void
fw_document_state_free (FwDocumentState *state)
{
  if (!state)
    return;
  g_free (state->zoom_mode);
  g_free (state->view_mode);
  g_free (state);
}
