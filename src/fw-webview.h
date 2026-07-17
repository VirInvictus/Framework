/* fw-webview.h — WebKitGTK-backed reader widget for reflow documents.
 *
 * Composite GtkWidget that hosts a WebKitWebView. Each FwWebView owns
 * an image lookup table (image_id → GBytes) that backs the
 * `framework-img:` URI scheme registered globally on the default
 * WebKitWebContext at first construction.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define FW_TYPE_WEBVIEW (fw_webview_get_type ())

G_DECLARE_FINAL_TYPE (FwWebView, fw_webview, FW, WEBVIEW, GtkWidget)

GtkWidget *fw_webview_new                (void);

/* Load an HTML document plus the lookup tables the framework-img:
 * scheme resolves against.  `images` is GHashTable<gchar* image_id,
 * GBytes*> for `framework-img://<doc-id>/<image-id>` references;
 * `resources` is GHashTable<gchar* zip_path, GBytes*> for
 * `framework-img://<doc-id>/res/<path>` references (publisher CSS,
 * fonts, and anything relative URLs inside those stylesheets reach).
 * FwWebView takes a reference on each and drops it on next load or on
 * dispose.  Pass NULL to clear either. `html` is the complete document;
 * its `<img>`/`<link>` URIs should already have been rewritten by the
 * caller (the doc-id for this view is fw_webview_get_doc_id). */
void       fw_webview_load_html          (FwWebView    *self,
                                          const char   *html,
                                          GHashTable   *images,
                                          GHashTable   *resources);

/* Per-view UUID-shaped string used as the host part of framework-img://
 * URIs.  Borrowed; valid for the lifetime of the FwWebView. */
const char *fw_webview_get_doc_id        (FwWebView    *self);

/* Scroll to the element with the given DOM id.  No-op if anchor is NULL
 * or the element isn't found. */
void       fw_webview_scroll_to_anchor   (FwWebView    *self,
                                          const char   *anchor);

/* dir == -1 page-back, +1 page-forward.  Mapped to a Page Up/Down style
 * window.scrollBy call. */
void       fw_webview_scroll_by_page     (FwWebView    *self,
                                          int           dir);

/* Asynchronously fetch the current reading position as JSON.  Callback
 * receives a g_strdup'd JSON string `{"anchor":"...", "scroll_y":N}` the
 * caller must free, or NULL on failure. */
typedef void (*FwWebViewPositionCb) (const char *json, gpointer user_data);
void       fw_webview_get_position       (FwWebView           *self,
                                          FwWebViewPositionCb  cb,
                                          gpointer             user_data);

/* Restore a previously-saved position JSON.  Queues until after the next
 * page load completes if necessary. */
void       fw_webview_restore_position   (FwWebView    *self,
                                          const char   *json);

/* Reading typography + theme, pushed live onto the document's `:root`
 * CSS custom properties (no reload). Any string left NULL or size <= 0
 * is left unchanged. Queued until the load finishes if it isn't ready. */
typedef struct {
  const char *body_font;    /* CSS font-family list */
  const char *mono_font;    /* CSS font-family list for code */
  double      font_size_pt; /* body font size in points */
  double      line_height;  /* unitless multiplier */
  const char *fg;           /* CSS foreground color */
  const char *bg;           /* CSS background color */
  const char *link;         /* CSS link color */
} FwReadingStyle;

void       fw_webview_set_reading_style  (FwWebView           *self,
                                          const FwReadingStyle *style);

/* Enable/disable publisher stylesheets (every <link rel=stylesheet> and
 * <style> except the built-in reading stylesheet) by flipping their DOM
 * .disabled flag — live, no reload. Queued until the load finishes if
 * the document isn't ready. */
void       fw_webview_set_publisher_styles (FwWebView *self,
                                            gboolean   enabled);

/* Dark-theme color transformation for publisher CSS: inverts the
 * lightness of author-set light backgrounds and dark text so a
 * publisher sheet doesn't glare against a dark reading theme. Pass TRUE
 * only when a dark reading theme is active; FALSE reverts it. Idempotent
 * and reversible; queued until load finishes if the document isn't
 * ready. */
void       fw_webview_set_dark_transform   (FwWebView *self,
                                            gboolean   on);

/* Latest reading position as JSON `{"anchor":"...","scroll_y":N}`, kept
 * current by a debounced in-page scroll listener that posts back to a
 * script-message handler.  Borrowed; valid until the next load or
 * dispose.  Returns NULL before the first scroll/load of the current
 * document.  Synchronous — meant for the save-on-teardown path where the
 * async fw_webview_get_position round-trip can't complete in time. */
const char *fw_webview_get_cached_position (FwWebView  *self);

/* WebKitFindController-backed search.  `needle` of NULL/empty clears
 * the highlight; the view emits "search-changed" once the match count
 * is known. */
void       fw_webview_set_search         (FwWebView    *self,
                                          const char   *needle);
void       fw_webview_find_next          (FwWebView    *self);
void       fw_webview_find_previous      (FwWebView    *self);
void       fw_webview_clear_search       (FwWebView    *self);

/* Total match count for the active search, populated after the
 * `search-changed` signal fires. */
guint      fw_webview_get_hit_count      (FwWebView    *self);

G_END_DECLS
