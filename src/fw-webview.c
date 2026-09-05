/* fw-webview.c — WebKitGTK-backed reader widget.
 *
 * Composite GtkWidget that hosts one WebKitWebView as its only child.
 * The `framework-img:` URI scheme is registered once globally on the
 * default WebKitWebContext at first construction; the handler resolves
 * `framework-img://<doc-id>/<image-id>` against a static registry that
 * each FwWebView populates with its own image bytes.
 *
 * The scheme handler runs on a WebKit IPC thread.  The registry and
 * each view's image table are accessed under `registry_lock` —
 * insertions and removals happen on the GTK main thread, lookups run
 * on the worker.  Image tables themselves are immutable for the
 * lifetime of the load (set by load_html, never mutated until the
 * next load), so the lock only protects the registry pointer; once
 * the worker holds a ref to the table it can iterate freely.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-webview.h"

#include <webkit/webkit.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define FW_IMG_SCHEME "framework-img"

/* Name of the script-message handler the in-page scroll listener posts
 * to (window.webkit.messageHandlers.<name>.postMessage). */
#define FW_POS_HANDLER "fwpos"

struct _FwWebView {
  GtkWidget          parent_instance;

  WebKitWebView     *web;             /* unowned (added as child) */
  WebKitFindController *finder;       /* borrowed from `web` */

  char              *doc_id;          /* per-view UUID, host of img URIs */
  GHashTable        *images;          /* gchar* id → GBytes*; owned ref */
  GHashTable        *resources;       /* gchar* zip-path → GBytes*; owned
                                       * ref; backs /res/<path> URIs */

  guint              hit_count;
  gboolean           load_done;       /* TRUE once load-changed FINISHED */
  char              *pending_position;/* restored if load_done == FALSE */
  char              *pending_anchor;  /* ditto for scroll_to_anchor */
  char              *pending_style;   /* reading-style JS, run on load */
  char              *pending_pub;     /* publisher-styles JS, run on load */
  char              *pending_dark;    /* dark color-transform JS, run on load
                                       * — after pending_pub, since it reads
                                       * computed styles post publisher toggle */
  char              *last_position;   /* latest {anchor,scroll_y,frac} JSON
                                       * from the in-page scroll listener;
                                       * read synchronously at save time */
  double             last_fraction;   /* 0..1 scroll progress parsed out of
                                       * the same message; drives the
                                       * header-bar percentage */
};

enum {
  SIG_LOAD_DONE,
  SIG_SEARCH_CHANGED,
  SIG_PROGRESS,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (FwWebView, fw_webview, GTK_TYPE_WIDGET)

/* ── Registry: doc-id → FwWebView* for the URI scheme handler ──────── */

static GMutex       registry_lock;
static GHashTable  *registry;          /* gchar* doc_id → FwWebView* (weak) */

static void
registry_add (FwWebView *self)
{
  g_mutex_lock (&registry_lock);
  if (!registry)
    registry = g_hash_table_new (g_str_hash, g_str_equal);
  /* The doc_id string lives on the FwWebView; safe to use as key by
   * pointer because the view stays alive until registry_remove. */
  g_hash_table_insert (registry, self->doc_id, self);
  g_mutex_unlock (&registry_lock);
}

static void
registry_remove (FwWebView *self)
{
  g_mutex_lock (&registry_lock);
  if (registry)
    g_hash_table_remove (registry, self->doc_id);
  g_mutex_unlock (&registry_lock);
}

/* Content-type sniff by magic bytes.  WebKit accepts any
 * image/(subtype) we hand it. */
static char *
sniff_mime (GBytes *bytes)
{
  gsize n;
  const guchar *p = g_bytes_get_data (bytes, &n);
  if      (n >= 8 && memcmp (p, "\x89PNG\r\n\x1a\n", 8) == 0) return g_strdup ("image/png");
  else if (n >= 3 && memcmp (p, "\xff\xd8\xff", 3) == 0)      return g_strdup ("image/jpeg");
  else if (n >= 6 && memcmp (p, "GIF87a", 6) == 0)            return g_strdup ("image/gif");
  else if (n >= 6 && memcmp (p, "GIF89a", 6) == 0)            return g_strdup ("image/gif");
  else if (n >= 4 && memcmp (p, "RIFF", 4) == 0)              return g_strdup ("image/webp");
  else if (n >= 4 && memcmp (p, "<svg", 4) == 0)              return g_strdup ("image/svg+xml");
  return g_strdup ("application/octet-stream");
}

/* MIME for /res/ paths: extension first (CSS and fonts have no reliable
 * magic WebKit trusts), magic sniff as fallback. */
static char *
resource_mime (const char *path, GBytes *bytes)
{
  const char *dot = strrchr (path, '.');
  if (dot) {
    if (g_ascii_strcasecmp (dot, ".css") == 0)   return g_strdup ("text/css");
    if (g_ascii_strcasecmp (dot, ".woff2") == 0) return g_strdup ("font/woff2");
    if (g_ascii_strcasecmp (dot, ".woff") == 0)  return g_strdup ("font/woff");
    if (g_ascii_strcasecmp (dot, ".ttf") == 0)   return g_strdup ("font/ttf");
    if (g_ascii_strcasecmp (dot, ".otf") == 0)   return g_strdup ("font/otf");
    if (g_ascii_strcasecmp (dot, ".svg") == 0)   return g_strdup ("image/svg+xml");
  }
  return sniff_mime (bytes);
}

/* Lookup runs on the WebKit IPC thread.  Returns a borrowed GBytes
 * reference; the caller must g_bytes_ref before unlocking. */
static GBytes *
registry_lookup_image (const char *doc_id, const char *image_id, char **out_mime)
{
  GBytes *bytes = NULL;
  g_mutex_lock (&registry_lock);
  FwWebView *view = registry ? g_hash_table_lookup (registry, doc_id) : NULL;
  if (view && view->images) {
    GBytes *src = g_hash_table_lookup (view->images, image_id);
    if (src)
      bytes = g_bytes_ref (src);
  }
  g_mutex_unlock (&registry_lock);
  if (bytes && out_mime)
    *out_mime = sniff_mime (bytes);
  return bytes;
}

/* /res/<zip-path> lookup (percent-decoded).  Serves publisher resources
 * (CSS, fonts, images referenced from CSS) by archive path so relative
 * url(...) and @import references inside stylesheets resolve natively
 * against the resource's own URI.  Scripts are never served: <script>
 * is stripped at emit, and refusing .js here keeps it that way even if
 * a future emit path slips. */
static GBytes *
registry_lookup_resource (const char *doc_id, const char *escaped_path,
                          char **out_mime)
{
  g_autofree char *path = g_uri_unescape_string (escaped_path, NULL);
  if (!path || !*path)
    return NULL;
  const char *dot = strrchr (path, '.');
  if (dot && (g_ascii_strcasecmp (dot, ".js") == 0 ||
              g_ascii_strcasecmp (dot, ".mjs") == 0))
    return NULL;

  GBytes *bytes = NULL;
  g_mutex_lock (&registry_lock);
  FwWebView *view = registry ? g_hash_table_lookup (registry, doc_id) : NULL;
  if (view && view->resources) {
    GBytes *src = g_hash_table_lookup (view->resources, path);
    if (src)
      bytes = g_bytes_ref (src);
  }
  g_mutex_unlock (&registry_lock);
  if (bytes && out_mime)
    *out_mime = resource_mime (path, bytes);
  return bytes;
}

/* ── URI scheme handler ────────────────────────────────────────────── */

/* Splits framework-img://<doc-id>/<image-id> into the two parts.  Both
 * returned pointers point into a g_strdup'd buffer the caller frees. */
static gboolean
parse_img_uri (const char *uri, char **out_doc_id, char **out_image_id, char **out_buf)
{
  const char *prefix = FW_IMG_SCHEME "://";
  gsize plen = strlen (prefix);
  if (!g_str_has_prefix (uri, prefix))
    return FALSE;
  char *buf = g_strdup (uri + plen);
  char *slash = strchr (buf, '/');
  if (!slash) { g_free (buf); return FALSE; }
  *slash = '\0';
  *out_doc_id   = buf;
  *out_image_id = slash + 1;
  *out_buf      = buf;
  return TRUE;
}

static void
on_img_scheme_request (WebKitURISchemeRequest *req, gpointer user_data G_GNUC_UNUSED)
{
  const char *uri = webkit_uri_scheme_request_get_uri (req);
  g_autofree char *buf = NULL;
  char *doc_id = NULL, *image_id = NULL;
  if (!parse_img_uri (uri, &doc_id, &image_id, &buf)) {
    g_autoptr (GError) err = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                          "bad framework-img URI: %s", uri);
    webkit_uri_scheme_request_finish_error (req, err);
    return;
  }

  g_autofree char *mime = NULL;
  GBytes *bytes;
  if (g_str_has_prefix (image_id, "res/"))
    bytes = registry_lookup_resource (doc_id, image_id + 4, &mime);
  else
    bytes = registry_lookup_image (doc_id, image_id, &mime);
  if (!bytes) {
    g_autoptr (GError) err = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                          "no resource %s in document %s",
                                          image_id, doc_id);
    webkit_uri_scheme_request_finish_error (req, err);
    return;
  }

  GInputStream *stream = g_memory_input_stream_new_from_bytes (bytes);
  gsize len = g_bytes_get_size (bytes);
  webkit_uri_scheme_request_finish (req, stream, (gint64) len, mime);
  g_object_unref (stream);
  g_bytes_unref (bytes);
}

static void
ensure_uri_scheme_registered (void)
{
  static gsize once = 0;
  if (g_once_init_enter (&once)) {
    WebKitWebContext *ctx = webkit_web_context_get_default ();
    webkit_web_context_register_uri_scheme (
      ctx, FW_IMG_SCHEME, on_img_scheme_request, NULL, NULL);
    /* Mark CORS-enabled so cross-origin fetches don't fail.  We
     * intentionally do NOT register as "local" — that would treat the
     * scheme like file:// and block document → image loads when the
     * document's own origin is anything else.  Same-origin between
     * document and image is achieved by passing a matching base_uri
     * to webkit_web_view_load_html instead. */
    WebKitSecurityManager *sec = webkit_web_context_get_security_manager (ctx);
    webkit_security_manager_register_uri_scheme_as_cors_enabled (sec, FW_IMG_SCHEME);
    g_once_init_leave (&once, 1);
  }
}

/* ── Search hooks ──────────────────────────────────────────────────── */

static void
on_found_text (WebKitFindController *fc,
               guint                 match_count,
               gpointer              user_data)
{
  FwWebView *self = FW_WEBVIEW (user_data);
  self->hit_count = match_count;
  g_signal_emit (self, signals[SIG_SEARCH_CHANGED], 0);
  (void) fc;
}

static void
on_failed_to_find_text (WebKitFindController *fc G_GNUC_UNUSED,
                        gpointer              user_data)
{
  FwWebView *self = FW_WEBVIEW (user_data);
  self->hit_count = 0;
  g_signal_emit (self, signals[SIG_SEARCH_CHANGED], 0);
}

static void
on_counted_matches (WebKitFindController *fc G_GNUC_UNUSED,
                    guint                 match_count,
                    gpointer              user_data)
{
  FwWebView *self = FW_WEBVIEW (user_data);
  self->hit_count = match_count;
  g_signal_emit (self, signals[SIG_SEARCH_CHANGED], 0);
}

/* ── Load lifecycle ────────────────────────────────────────────────── */

static void
flush_pending_after_load (FwWebView *self)
{
  if (self->pending_anchor) {
    g_autofree char *anchor = g_steal_pointer (&self->pending_anchor);
    fw_webview_scroll_to_anchor (self, anchor);
  }
  if (self->pending_position) {
    g_autofree char *json = g_steal_pointer (&self->pending_position);
    fw_webview_restore_position (self, json);
  }
  if (self->pending_style) {
    g_autofree char *js = g_steal_pointer (&self->pending_style);
    webkit_web_view_evaluate_javascript (
      self->web, js, -1, NULL, NULL, NULL, NULL, NULL);
  }
  if (self->pending_pub) {
    g_autofree char *js = g_steal_pointer (&self->pending_pub);
    webkit_web_view_evaluate_javascript (
      self->web, js, -1, NULL, NULL, NULL, NULL, NULL);
  }
  if (self->pending_dark) {
    g_autofree char *js = g_steal_pointer (&self->pending_dark);
    webkit_web_view_evaluate_javascript (
      self->web, js, -1, NULL, NULL, NULL, NULL, NULL);
  }
}

static void
on_load_changed (WebKitWebView *web G_GNUC_UNUSED,
                 WebKitLoadEvent ev,
                 gpointer user_data)
{
  FwWebView *self = FW_WEBVIEW (user_data);
  if (ev == WEBKIT_LOAD_FINISHED) {
    self->load_done = TRUE;
    flush_pending_after_load (self);
    g_signal_emit (self, signals[SIG_LOAD_DONE], 0);
  } else if (ev == WEBKIT_LOAD_STARTED) {
    self->load_done = FALSE;
  }
}

/* ── Position tracking ─────────────────────────────────────────────── */

/* A debounced scroll/load listener (injected as a user script) posts the
 * current {anchor, scroll_y} here as a JSON string.  We cache the latest
 * so the save-on-teardown path can read it synchronously — the async
 * fw_webview_get_position round-trip can't finish during dispose. */
static void
on_position_message (WebKitUserContentManager *ucm G_GNUC_UNUSED,
                     JSCValue                 *value,
                     gpointer                  user_data)
{
  FwWebView *self = FW_WEBVIEW (user_data);
  if (!value || !jsc_value_is_string (value))
    return;
  g_free (self->last_position);
  self->last_position = jsc_value_to_string (value);

  /* Pull the scroll fraction out of the message for the header-bar
   * percentage. json-glib only enters through here — the save/restore
   * path still treats last_position as an opaque string. */
  JsonParser *parser = json_parser_new ();
  if (json_parser_load_from_data (parser, self->last_position, -1, NULL)) {
    JsonNode *root = json_parser_get_root (parser);
    if (root && JSON_NODE_HOLDS_OBJECT (root)) {
      JsonObject *obj = json_node_get_object (root);
      double frac = -1.0;
      if (json_object_has_member (obj, "frac"))
        frac = json_object_get_double_member (obj, "frac");
      if (frac < 0.0) frac = 0.0;
      if (frac > 1.0) frac = 1.0;
      self->last_fraction = frac;
      g_signal_emit (self, signals[SIG_PROGRESS], 0);
    }
  }
  g_object_unref (parser);
}

/* User script: a passive, 200ms-debounced scroll listener (plus one shot
 * on load) that reports the topmost element with an id and the scroll
 * offset.  Mirrors the query fw_webview_get_position runs on demand. */
static const char FW_POS_USER_SCRIPT[] =
  "(function () {"
  "  function snap() {"
  "    var se = document.scrollingElement || document.documentElement;"
  "    var y = se ? se.scrollTop : window.scrollY;"
  "    var max = se ? (se.scrollHeight - window.innerHeight) : 0;"
  "    var f = max > 0 ? y / max : 0;"
  "    f = f < 0 ? 0 : (f > 1 ? 1 : f);"
  "    var a = null;"
  "    var nodes = document.querySelectorAll('[id]');"
  "    for (var i = 0; i < nodes.length; i++) {"
  "      var r = nodes[i].getBoundingClientRect();"
  "      if (r.top >= 0) { a = nodes[i].id; break; }"
  "    }"
  "    try { window.webkit.messageHandlers." FW_POS_HANDLER
  "          .postMessage(JSON.stringify({ anchor: a, scroll_y: y, frac: f })); } catch (e) {}"
  "  }"
  "  var t = null;"
  "  window.addEventListener('scroll', function () {"
  "    if (t) clearTimeout(t);"
  "    t = setTimeout(snap, 200);"
  "  }, { passive: true });"
  "  window.addEventListener('load', snap);"
  "})();";

/* ── Navigation policy ─────────────────────────────────────────────────
 *
 * The stitched document is the whole reading session: navigating the
 * WebView anywhere else destroys it (an unresolved relative href used
 * to load an error page from the img scheme handler).  Policy:
 *   - same-document fragment jumps (rewritten in-book links): allowed;
 *   - http(s)/mailto link clicks: opened in the default browser via
 *     GtkUriLauncher, navigation blocked;
 *   - any other link-click navigation: blocked;
 *   - non-link navigations (the load_html itself): allowed.  In-book
 *     scripts are stripped at emit, so nothing else can navigate. */

/* target and current URIs match up to the fragment → same-document. */
static gboolean
is_same_document (const char *target, const char *current)
{
  if (!target || !current)
    return FALSE;
  gsize n = 0;
  while (target[n] && target[n] != '#' &&
         current[n] && current[n] != '#' && target[n] == current[n])
    n++;
  gboolean t_end = target[n] == '\0' || target[n] == '#';
  gboolean c_end = current[n] == '\0' || current[n] == '#';
  return t_end && c_end;
}

static gboolean
on_decide_policy (WebKitWebView            *web,
                  WebKitPolicyDecision     *decision,
                  WebKitPolicyDecisionType  type,
                  gpointer                  user_data)
{
  FwWebView *self = FW_WEBVIEW (user_data);
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
    return FALSE;   /* default handling */

  WebKitNavigationPolicyDecision *nav =
    WEBKIT_NAVIGATION_POLICY_DECISION (decision);
  WebKitNavigationAction *action =
    webkit_navigation_policy_decision_get_navigation_action (nav);
  if (webkit_navigation_action_get_navigation_type (action) !=
      WEBKIT_NAVIGATION_TYPE_LINK_CLICKED)
    return FALSE;   /* load_html, reload, ... — allow */

  const char *uri =
    webkit_uri_request_get_uri (webkit_navigation_action_get_request (action));
  if (is_same_document (uri, webkit_web_view_get_uri (web)))
    return FALSE;   /* fragment jump inside the book — allow */

  if (uri && (g_str_has_prefix (uri, "http://")  ||
              g_str_has_prefix (uri, "https://") ||
              g_str_has_prefix (uri, "mailto:"))) {
    GtkUriLauncher *launcher = gtk_uri_launcher_new (uri);
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
    gtk_uri_launcher_launch (launcher, GTK_IS_WINDOW (root) ?
                             GTK_WINDOW (root) : NULL, NULL, NULL, NULL);
    g_object_unref (launcher);
  }
  webkit_policy_decision_ignore (decision);
  return TRUE;
}

/* ── Remote-content blocker ────────────────────────────────────────────
 *
 * Books are local documents; nothing in one should reach the network
 * (remote images and stylesheets are tracking vectors).  A compiled
 * WebKit content filter blocks every http(s)/ws(s) subresource fetch;
 * top-level navigation is already handled by the policy hook above.
 * Compilation is async and cached by WebKit under the store path, so
 * after the first run this resolves from disk immediately. */

static const char REMOTE_BLOCK_RULES[] =
  "[{\"trigger\":{\"url-filter\":\"^https?:\"},\"action\":{\"type\":\"block\"}},"
  " {\"trigger\":{\"url-filter\":\"^wss?:\"},\"action\":{\"type\":\"block\"}}]";

static void
on_filter_saved (GObject *src, GAsyncResult *res, gpointer user_data)
{
  WebKitUserContentManager *ucm = user_data;
  g_autoptr (GError) err = NULL;
  WebKitUserContentFilter *filter =
    webkit_user_content_filter_store_save_finish (
      WEBKIT_USER_CONTENT_FILTER_STORE (src), res, &err);
  if (filter) {
    webkit_user_content_manager_add_filter (ucm, filter);
    webkit_user_content_filter_unref (filter);
  } else {
    g_warning ("fw-webview: remote-block filter failed to compile: %s",
               err ? err->message : "(unknown)");
  }
  g_object_unref (ucm);
}

static void
install_remote_block_filter (WebKitUserContentManager *ucm)
{
  g_autofree char *dir =
    g_build_filename (g_get_user_cache_dir (), "framework",
                      "content-filters", NULL);
  g_mkdir_with_parents (dir, 0700);
  WebKitUserContentFilterStore *store =
    webkit_user_content_filter_store_new (dir);
  g_autoptr (GBytes) rules =
    g_bytes_new_static (REMOTE_BLOCK_RULES, sizeof (REMOTE_BLOCK_RULES) - 1);
  webkit_user_content_filter_store_save (
    store, "fw-block-remote", rules, NULL,
    on_filter_saved, g_object_ref (ucm));
  g_object_unref (store);
}

/* ── Widget machinery ──────────────────────────────────────────────── */

static char *
gen_doc_id (void)
{
  /* GUID-ish: 8 random hex bytes are sufficient to distinguish a
   * handful of open windows; we don't need full-RFC entropy. */
  guint64 r1 = g_random_int (); r1 = (r1 << 32) | g_random_int ();
  return g_strdup_printf ("doc-%016" G_GINT64_MODIFIER "x", r1);
}

static void
fw_webview_dispose (GObject *object)
{
  FwWebView *self = FW_WEBVIEW (object);
  registry_remove (self);
  g_clear_pointer (&self->images, g_hash_table_unref);
  g_clear_pointer (&self->resources, g_hash_table_unref);
  g_clear_pointer (&self->pending_anchor, g_free);
  g_clear_pointer (&self->pending_position, g_free);
  g_clear_pointer (&self->pending_style, g_free);
  g_clear_pointer (&self->pending_pub, g_free);
  g_clear_pointer (&self->pending_dark, g_free);
  g_clear_pointer (&self->last_position, g_free);
  if (self->web) {
    gtk_widget_unparent (GTK_WIDGET (self->web));
    self->web    = NULL;
    self->finder = NULL;
  }
  G_OBJECT_CLASS (fw_webview_parent_class)->dispose (object);
}

static void
fw_webview_finalize (GObject *object)
{
  FwWebView *self = FW_WEBVIEW (object);
  g_clear_pointer (&self->doc_id, g_free);
  G_OBJECT_CLASS (fw_webview_parent_class)->finalize (object);
}

static void
fw_webview_class_init (FwWebViewClass *klass)
{
  GObjectClass *o_class = G_OBJECT_CLASS (klass);
  o_class->dispose  = fw_webview_dispose;
  o_class->finalize = fw_webview_finalize;
  GtkWidgetClass *w_class = GTK_WIDGET_CLASS (klass);
  gtk_widget_class_set_layout_manager_type (w_class, GTK_TYPE_BIN_LAYOUT);

  /* Fired when WebKit reports load-changed FINISHED. */
  signals[SIG_LOAD_DONE] = g_signal_new (
    "load-done", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /* Fired when hit_count changes (search-text result, count-matches,
   * or failed-to-find).  Consumers read hit_count via the getter. */
  signals[SIG_SEARCH_CHANGED] = g_signal_new (
    "search-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /* Fired when the in-page scroll listener posts a new position;
   * consumers read fw_webview_get_scroll_fraction for the header
   * percentage. */
  signals[SIG_PROGRESS] = g_signal_new (
    "progress-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
fw_webview_init (FwWebView *self)
{
  ensure_uri_scheme_registered ();

  self->doc_id = gen_doc_id ();
  registry_add (self);

  self->web    = WEBKIT_WEB_VIEW (webkit_web_view_new ());
  self->finder = webkit_web_view_get_find_controller (self->web);

  /* Position tracking: register the script-message handler and inject the
   * scroll listener that feeds it.  Must be set up before any load so the
   * user script is installed for the first document. */
  WebKitUserContentManager *ucm =
    webkit_web_view_get_user_content_manager (self->web);
  webkit_user_content_manager_register_script_message_handler (
    ucm, FW_POS_HANDLER, NULL);
  g_signal_connect (ucm, "script-message-received::" FW_POS_HANDLER,
                    G_CALLBACK (on_position_message), self);
  WebKitUserScript *pos_script = webkit_user_script_new (
    FW_POS_USER_SCRIPT,
    WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
    NULL, NULL);
  webkit_user_content_manager_add_script (ucm, pos_script);
  webkit_user_script_unref (pos_script);

  install_remote_block_filter (ucm);

  /* JS is required for our scroll_to_anchor / restore_position helpers.
   * Local-storage and HTML5 database are off — viewer, not a browser. */
  WebKitSettings *s = webkit_web_view_get_settings (self->web);
  webkit_settings_set_enable_javascript          (s, TRUE);
  webkit_settings_set_enable_developer_extras    (s, FALSE);
  webkit_settings_set_enable_html5_database      (s, FALSE);
  webkit_settings_set_enable_html5_local_storage (s, FALSE);

  gtk_widget_set_hexpand (GTK_WIDGET (self->web), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->web), TRUE);
  gtk_widget_set_parent  (GTK_WIDGET (self->web), GTK_WIDGET (self));

  g_signal_connect (self->web, "load-changed",
                    G_CALLBACK (on_load_changed), self);
  g_signal_connect (self->web, "decide-policy",
                    G_CALLBACK (on_decide_policy), self);
  g_signal_connect (self->finder, "found-text",
                    G_CALLBACK (on_found_text), self);
  g_signal_connect (self->finder, "failed-to-find-text",
                    G_CALLBACK (on_failed_to_find_text), self);
  g_signal_connect (self->finder, "counted-matches",
                    G_CALLBACK (on_counted_matches), self);
}

/* ── Public API ────────────────────────────────────────────────────── */

GtkWidget *
fw_webview_new (void)
{
  return g_object_new (FW_TYPE_WEBVIEW, NULL);
}

const char *
fw_webview_get_doc_id (FwWebView *self)
{
  g_return_val_if_fail (FW_IS_WEBVIEW (self), NULL);
  return self->doc_id;
}

void
fw_webview_load_html (FwWebView *self, const char *html, GHashTable *images,
                      GHashTable *resources)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  g_return_if_fail (html != NULL);

  /* Swap the image + resource tables.  The scheme handler may already
   * be resolving a URI from the previous load; the lock in the
   * registry_lookup_* helpers synchronizes with us here. */
  g_mutex_lock (&registry_lock);
  if (self->images != images) {
    if (self->images) g_hash_table_unref (self->images);
    self->images = images ? g_hash_table_ref (images) : NULL;
  }
  if (self->resources != resources) {
    if (self->resources) g_hash_table_unref (self->resources);
    self->resources = resources ? g_hash_table_ref (resources) : NULL;
  }
  g_mutex_unlock (&registry_lock);

  self->load_done = FALSE;
  /* Drop the previous document's cached position so a save before the
   * new document scrolls can't persist a stale anchor against it. */
  g_clear_pointer (&self->last_position, g_free);
  g_clear_pointer (&self->pending_style, g_free);
  g_clear_pointer (&self->pending_pub, g_free);
  g_clear_pointer (&self->pending_dark, g_free);
  /* base_uri matches the framework-img: origin our images live in so
   * the document and images are same-origin.  Without this, WebKit's
   * mixed-origin rules silently drop the image fetches before they
   * reach our scheme handler — the image element renders its alt text
   * and the handler never sees the request.  The base path itself
   * doesn't need to resolve to anything; WebKit just uses it for the
   * security origin computation. */
  g_autofree char *base_uri =
    g_strdup_printf ("%s://%s/", FW_IMG_SCHEME, self->doc_id);
  webkit_web_view_load_html (self->web, html, base_uri);
}

void
fw_webview_scroll_to_anchor (FwWebView *self, const char *anchor)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  if (!anchor || !*anchor) return;

  if (!self->load_done) {
    /* Hold for after load-changed FINISHED. */
    g_free (self->pending_anchor);
    self->pending_anchor = g_strdup (anchor);
    return;
  }

  /* JS escape: anchor IDs from EPUBs are XHTML name-tokens; allow only
   * letters/digits/_/-/. Anything else gets stripped to keep us safe
   * from injection. */
  g_autoptr (GString) safe = g_string_new (NULL);
  for (const char *p = anchor; *p; p++) {
    if (g_ascii_isalnum (*p) || *p == '_' || *p == '-' || *p == '.' || *p == ':')
      g_string_append_c (safe, *p);
  }
  if (safe->len == 0) return;

  g_autofree char *script = g_strdup_printf (
    "var e = document.getElementById('%s');"
    "if (e) e.scrollIntoView({block:'start'});",
    safe->str);
  webkit_web_view_evaluate_javascript (
    self->web, script, -1, NULL, NULL, NULL, NULL, NULL);
}

void
fw_webview_scroll_by_page (FwWebView *self, int dir)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  if (dir == 0) return;
  const char *sign = (dir < 0) ? "-" : "";
  g_autofree char *script = g_strdup_printf (
    "window.scrollBy({top: %swindow.innerHeight * 0.92, behavior: 'instant'});",
    sign);
  webkit_web_view_evaluate_javascript (
    self->web, script, -1, NULL, NULL, NULL, NULL, NULL);
}

/* ── Position get/restore (filled in at Step 5) ────────────────────── */

typedef struct {
  FwWebViewPositionCb cb;
  gpointer            user_data;
} PositionRequest;

static void
on_position_js_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  PositionRequest *pr = user_data;
  WebKitWebView *web = WEBKIT_WEB_VIEW (src);
  g_autoptr (GError) err = NULL;
  g_autoptr (JSCValue) v =
    webkit_web_view_evaluate_javascript_finish (web, res, &err);
  if (!v || err) {
    pr->cb (NULL, pr->user_data);
  } else {
    g_autofree char *json = jsc_value_to_json (v, 0);
    pr->cb (json, pr->user_data);
  }
  g_free (pr);
}

void
fw_webview_get_position (FwWebView           *self,
                         FwWebViewPositionCb  cb,
                         gpointer             user_data)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  g_return_if_fail (cb != NULL);
  if (!self->load_done) { cb (NULL, user_data); return; }

  const char *script =
    "(function () {"
    "  var se = document.scrollingElement || document.documentElement;"
    "  var y = se ? se.scrollTop : window.scrollY;"
    "  var max = se ? (se.scrollHeight - window.innerHeight) : 0;"
    "  var f = max > 0 ? y / max : 0;"
    "  f = f < 0 ? 0 : (f > 1 ? 1 : f);"
    "  var a = null;"
    "  var nodes = document.querySelectorAll('[id]');"
    "  for (var i = 0; i < nodes.length; i++) {"
    "    var r = nodes[i].getBoundingClientRect();"
    "    if (r.top >= 0) { a = nodes[i].id; break; }"
    "  }"
    "  return { anchor: a, scroll_y: y, frac: f };"
    "})()";
  PositionRequest *pr = g_new0 (PositionRequest, 1);
  pr->cb        = cb;
  pr->user_data = user_data;
  webkit_web_view_evaluate_javascript (
    self->web, script, -1, NULL, NULL, NULL, on_position_js_done, pr);
}

void
fw_webview_restore_position (FwWebView *self, const char *json)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  if (!json || !*json) return;
  if (!self->load_done) {
    g_free (self->pending_position);
    self->pending_position = g_strdup (json);
    return;
  }
  /* Parse JSON in JS — avoids dragging json-glib into this file just to
   * marshal back and forth.  Sanitise the input to a JS object literal
   * by quoting it; JSON is a valid JS expression. */
  g_autofree char *script = g_strdup_printf (
    "(function () {"
    "  var p = %s;"
    "  if (p.anchor) {"
    "    var e = document.getElementById(p.anchor);"
    "    if (e) { e.scrollIntoView({block:'start'}); return; }"
    "  }"
    "  if (typeof p.scroll_y === 'number') window.scrollTo(0, p.scroll_y);"
    "})()", json);
  webkit_web_view_evaluate_javascript (
    self->web, script, -1, NULL, NULL, NULL, NULL, NULL);
}

const char *
fw_webview_get_cached_position (FwWebView *self)
{
  g_return_val_if_fail (FW_IS_WEBVIEW (self), NULL);
  return self->last_position;
}

double
fw_webview_get_scroll_fraction (FwWebView *self)
{
  g_return_val_if_fail (FW_IS_WEBVIEW (self), 0.0);
  return self->last_fraction;
}

/* Append a `r.setProperty('--name', '<value>')` call, escaping the value
 * for a single-quoted JS string literal (CSS values are letters, digits,
 * #, %, commas, spaces, dots, quotes — only ' and \ need escaping). */
static void
append_set_prop (GString *js, const char *name, const char *value)
{
  if (!value)
    return;
  g_string_append_printf (js, "r.setProperty('%s','", name);
  for (const char *p = value; *p; p++) {
    if (*p == '\'' || *p == '\\')
      g_string_append_c (js, '\\');
    g_string_append_c (js, *p);
  }
  g_string_append (js, "');");
}

void
fw_webview_set_reading_style (FwWebView *self, const FwReadingStyle *s)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  g_return_if_fail (s != NULL);

  g_autoptr (GString) js = g_string_new (
    "(function(){var r=document.documentElement.style;");
  append_set_prop (js, "--body-font", s->body_font);
  append_set_prop (js, "--mono-font", s->mono_font);
  if (s->font_size_pt > 0) {
    g_autofree char *v = g_strdup_printf ("%.1fpt", s->font_size_pt);
    append_set_prop (js, "--font-size", v);
  }
  if (s->line_height > 0) {
    g_autofree char *v = g_strdup_printf ("%.3f", s->line_height);
    append_set_prop (js, "--line-height", v);
  }
  append_set_prop (js, "--fg",   s->fg);
  append_set_prop (js, "--bg",   s->bg);
  append_set_prop (js, "--link", s->link);
  g_string_append (js, "})();");

  if (!self->load_done) {
    /* Latest style wins; flushed after load-changed FINISHED. */
    g_free (self->pending_style);
    self->pending_style = g_string_free (g_steal_pointer (&js), FALSE);
    return;
  }
  webkit_web_view_evaluate_javascript (
    self->web, js->str, -1, NULL, NULL, NULL, NULL, NULL);
}

void
fw_webview_set_publisher_styles (FwWebView *self, gboolean enabled)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));

  /* Publisher CSS is always emitted (as <link>s and class="fw-pub"
   * style blocks); the toggle flips their DOM .disabled flag, which is
   * instant and reversible without re-producing the document.  The
   * reading stylesheet (id fw-reading-css) is never touched. */
  g_autofree char *js = g_strdup_printf (
    "(function (on) {"
    "  var els = document.querySelectorAll('link[rel=\"stylesheet\"],style');"
    "  for (var i = 0; i < els.length; i++)"
    "    if (els[i].id !== 'fw-reading-css') els[i].disabled = !on;"
    "})(%s);", enabled ? "true" : "false");

  if (!self->load_done) {
    g_free (self->pending_pub);
    self->pending_pub = g_steal_pointer (&js);
    return;
  }
  webkit_web_view_evaluate_javascript (
    self->web, js, -1, NULL, NULL, NULL, NULL, NULL);
}

/* Dark-theme publisher-CSS color transformation (Calibre-style).
 *
 * A publisher stylesheet that paints an explicit light background on an
 * inner container glares against a dark reading theme, and dark author
 * text on a container we darken would vanish. The reading CSS's
 * !important body rules only cover html/body, not these inner elements.
 *
 * This walks the document and inverts the *lightness* (in HSL, keeping
 * hue and saturation) of two author-specified color kinds that are on
 * the wrong side for dark mode: opaque light backgrounds → dark, and
 * dark text → light. Correct colors (transparent backgrounds, already
 * light themed text, our forced link color) are left untouched, so a
 * light-blue callout becomes a dark-blue callout rather than being
 * flattened. It runs in two passes — read every computed color, then
 * apply — so there's no read/write layout thrash, and it's idempotent
 * and reversible: each changed element is tagged data-fw-dark, reverted
 * on the next call before re-applying (or when `on` is FALSE). */
void
fw_webview_set_dark_transform (FwWebView *self, gboolean on)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));

  g_autofree char *js = g_strdup_printf (
    "(function(on){"
    "  var old=document.querySelectorAll('[data-fw-dark]');"
    "  for(var i=0;i<old.length;i++){var w=old[i].getAttribute('data-fw-dark');"
    "    if(w.indexOf('b')>=0)old[i].style.removeProperty('background-color');"
    "    if(w.indexOf('f')>=0)old[i].style.removeProperty('color');"
    "    old[i].removeAttribute('data-fw-dark');}"
    "  if(!on||!document.body)return;"
    "  function parse(c){var m=c.match(/rgba?\\(([\\d.,\\s]+)\\)/);if(!m)return null;"
    "    var p=m[1].split(',').map(parseFloat);"
    "    return{r:p[0],g:p[1],b:p[2],a:p.length>3?p[3]:1};}"
    "  function lum(c){return(0.2126*c.r+0.7152*c.g+0.0722*c.b)/255;}"
    "  function invL(c){var r=c.r/255,g=c.g/255,b=c.b/255,"
    "    mx=Math.max(r,g,b),mn=Math.min(r,g,b),l=(mx+mn)/2,h=0,s=0;"
    "    if(mx!==mn){var d=mx-mn;s=l>0.5?d/(2-mx-mn):d/(mx+mn);"
    "      h=mx===r?(g-b)/d+(g<b?6:0):mx===g?(b-r)/d+2:(r-g)/d+4;h/=6;}"
    "    l=1-l;"
    "    function hq(p,q,t){if(t<0)t+=1;if(t>1)t-=1;"
    "      if(t<1/6)return p+(q-p)*6*t;if(t<0.5)return q;"
    "      if(t<2/3)return p+(q-p)*(2/3-t)*6;return p;}"
    "    var Q=l<0.5?l*(1+s):l+s-l*s,P=2*l-Q,"
    "      R=s?hq(P,Q,h+1/3):l,G=s?hq(P,Q,h):l,B=s?hq(P,Q,h-1/3):l;"
    "    return'rgb('+Math.round(R*255)+','+Math.round(G*255)+','+Math.round(B*255)+')';}"
    "  var all=document.body.getElementsByTagName('*'),jobs=[];"
    "  for(var i=0;i<all.length;i++){var el=all[i],cs=getComputedStyle(el),"
    "    bg=parse(cs.backgroundColor),fg=parse(cs.color),nb=null,nf=null,tag='';"
    "    if(bg&&bg.a>=0.4&&lum(bg)>0.55){nb=invL(bg);tag+='b';}"
    "    if(fg&&fg.a>=0.4&&lum(fg)<0.45){nf=invL(fg);tag+='f';}"
    "    if(tag)jobs.push([el,nb,nf,tag]);}"
    "  for(var j=0;j<jobs.length;j++){var e=jobs[j][0];"
    "    if(jobs[j][1])e.style.setProperty('background-color',jobs[j][1],'important');"
    "    if(jobs[j][2])e.style.setProperty('color',jobs[j][2],'important');"
    "    e.setAttribute('data-fw-dark',jobs[j][3]);}"
    "})(%s);", on ? "true" : "false");

  if (!self->load_done) {
    g_free (self->pending_dark);
    self->pending_dark = g_steal_pointer (&js);
    return;
  }
  webkit_web_view_evaluate_javascript (
    self->web, js, -1, NULL, NULL, NULL, NULL, NULL);
}

/* ── Search ────────────────────────────────────────────────────────── */

void
fw_webview_set_search (FwWebView *self, const char *needle)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  if (!needle || !*needle) {
    fw_webview_clear_search (self);
    return;
  }
  /* WEBKIT_FIND_OPTIONS_NONE matches case-sensitively by default;
   * CASE_INSENSITIVE + WRAP_AROUND mirror what the fixed-layout search
   * does for PDF. */
  guint32 opts = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE
               | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
  webkit_find_controller_count_matches (self->finder, needle, opts, G_MAXUINT);
  webkit_find_controller_search        (self->finder, needle, opts, G_MAXUINT);
}

void
fw_webview_find_next (FwWebView *self)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  webkit_find_controller_search_next (self->finder);
}

void
fw_webview_find_previous (FwWebView *self)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  webkit_find_controller_search_previous (self->finder);
}

void
fw_webview_clear_search (FwWebView *self)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  webkit_find_controller_search_finish (self->finder);
  if (self->hit_count != 0) {
    self->hit_count = 0;
    g_signal_emit (self, signals[SIG_SEARCH_CHANGED], 0);
  }
}

guint
fw_webview_get_hit_count (FwWebView *self)
{
  g_return_val_if_fail (FW_IS_WEBVIEW (self), 0);
  return self->hit_count;
}
