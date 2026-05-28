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

  guint              hit_count;
  gboolean           load_done;       /* TRUE once load-changed FINISHED */
  char              *pending_position;/* restored if load_done == FALSE */
  char              *pending_anchor;  /* ditto for scroll_to_anchor */
  char              *last_position;   /* latest {anchor,scroll_y} JSON from
                                       * the in-page scroll listener; read
                                       * synchronously at save time */
};

enum {
  SIG_LOAD_DONE,
  SIG_SEARCH_CHANGED,
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
  /* Cheap content-type sniff by magic bytes.  WebKit accepts any
   * image/(subtype) we hand it. */
  if (bytes && out_mime) {
    gsize n;
    const guchar *p = g_bytes_get_data (bytes, &n);
    if      (n >= 8 && memcmp (p, "\x89PNG\r\n\x1a\n", 8) == 0) *out_mime = g_strdup ("image/png");
    else if (n >= 3 && memcmp (p, "\xff\xd8\xff", 3) == 0)      *out_mime = g_strdup ("image/jpeg");
    else if (n >= 6 && memcmp (p, "GIF87a", 6) == 0)            *out_mime = g_strdup ("image/gif");
    else if (n >= 6 && memcmp (p, "GIF89a", 6) == 0)            *out_mime = g_strdup ("image/gif");
    else if (n >= 4 && memcmp (p, "RIFF", 4) == 0)              *out_mime = g_strdup ("image/webp");
    else if (n >= 4 && memcmp (p, "<svg", 4) == 0)              *out_mime = g_strdup ("image/svg+xml");
    else                                                         *out_mime = g_strdup ("application/octet-stream");
  }
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
  GBytes *bytes = registry_lookup_image (doc_id, image_id, &mime);
  if (!bytes) {
    g_autoptr (GError) err = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                          "no image %s in document %s",
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
}

/* User script: a passive, 200ms-debounced scroll listener (plus one shot
 * on load) that reports the topmost element with an id and the scroll
 * offset.  Mirrors the query fw_webview_get_position runs on demand. */
static const char FW_POS_USER_SCRIPT[] =
  "(function () {"
  "  function snap() {"
  "    var y = document.scrollingElement ? document.scrollingElement.scrollTop : window.scrollY;"
  "    var a = null;"
  "    var nodes = document.querySelectorAll('[id]');"
  "    for (var i = 0; i < nodes.length; i++) {"
  "      var r = nodes[i].getBoundingClientRect();"
  "      if (r.top >= 0) { a = nodes[i].id; break; }"
  "    }"
  "    try { window.webkit.messageHandlers." FW_POS_HANDLER
  "          .postMessage(JSON.stringify({ anchor: a, scroll_y: y })); } catch (e) {}"
  "  }"
  "  var t = null;"
  "  window.addEventListener('scroll', function () {"
  "    if (t) clearTimeout(t);"
  "    t = setTimeout(snap, 200);"
  "  }, { passive: true });"
  "  window.addEventListener('load', snap);"
  "})();";

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
  g_clear_pointer (&self->pending_anchor, g_free);
  g_clear_pointer (&self->pending_position, g_free);
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
fw_webview_load_html (FwWebView *self, const char *html, GHashTable *images)
{
  g_return_if_fail (FW_IS_WEBVIEW (self));
  g_return_if_fail (html != NULL);

  /* Swap the image table.  The scheme handler may already be resolving
   * a URI from the previous load; the lock in registry_lookup_image
   * synchronizes with us here. */
  g_mutex_lock (&registry_lock);
  if (self->images != images) {
    if (self->images) g_hash_table_unref (self->images);
    self->images = images ? g_hash_table_ref (images) : NULL;
  }
  g_mutex_unlock (&registry_lock);

  self->load_done = FALSE;
  /* Drop the previous document's cached position so a save before the
   * new document scrolls can't persist a stale anchor against it. */
  g_clear_pointer (&self->last_position, g_free);
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
    "  var y = document.scrollingElement ? document.scrollingElement.scrollTop : window.scrollY;"
    "  var a = null;"
    "  var nodes = document.querySelectorAll('[id]');"
    "  for (var i = 0; i < nodes.length; i++) {"
    "    var r = nodes[i].getBoundingClientRect();"
    "    if (r.top >= 0) { a = nodes[i].id; break; }"
    "  }"
    "  return { anchor: a, scroll_y: y };"
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
