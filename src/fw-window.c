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
#include <glib/gstdio.h>

typedef struct {
  int     page;
  double  fraction;  /* 0..1 within page */
} NavEntry;

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
  AdwToastOverlay      *toast_overlay;  /* wraps content; hosts AdwToasts */
  AdwOverlaySplitView  *split_view;
  FwSidebar            *sidebar;
  GtkStack             *content_stack;  /* "empty" | "document" */
  GtkScrolledWindow    *scroll;
  FwView               *view;
  FwSearch             *search;
  GtkSearchBar         *search_bar;
  GtkSearchEntry       *search_entry;
  GtkLabel             *search_count_label;
  GtkButton            *search_prev_button;
  GtkButton            *search_next_button;

  /* State */
  double                zoom;
  int                   rotation;
  int                   current_page;
  gboolean              invert_colors;
  char                 *file_path;   /* absolute path of current document */

  /* Deferred restore */
  double                _restore_scroll;  /* scroll fraction to restore */
  gboolean              _restore_pending;

  /* Navigation history — stacks of NavEntry (page, intra-page-fraction).
   * Pushed only on explicit jumps (link click, sidebar TOC, page entry),
   * not on plain scroll or next/prev page actions. Cleared on document
   * change. */
  GArray               *nav_back;     /* NavEntry[] */
  GArray               *nav_forward;  /* NavEntry[] */
  gboolean              nav_in_progress;  /* re-entrancy guard */

  /* Auto-reload (Phase 14): GFileMonitor watches the open document
   * path. When the file changes (LaTeX recompile, Typst rebuild, etc.)
   * we save state, re-open the document, and restore scroll position
   * — matching SumatraPDF's killer feature for the LaTeX/Typst flow.
   * Monitor is replaced on every fw_window_open_file. */
  GFileMonitor         *file_monitor;
  guint                 reload_debounce_id;  /* g_timeout source for debouncing
                                              * rapid CHANGED events during a write */

  /* Settings — kinetic-scrolling drives gtk_scrolled_window_set_kinetic_scrolling
   * on self->scroll. The view's scroll-related setting fields are gone now
   * that we don't intercept scroll events; this is the single point of
   * truth for the kinetic toggle. */
  GSettings            *settings;
  gulong                settings_kinetic_handler;
};

G_DEFINE_FINAL_TYPE (FwWindow, fw_window, ADW_TYPE_APPLICATION_WINDOW)

static void fw_window_save_state           (FwWindow *self);
static void nav_push_current               (FwWindow *self);
static void on_kinetic_setting_changed     (GSettings *settings,
                                            const char *key,
                                            gpointer user_data);

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
  if (self->sidebar)
    fw_sidebar_set_current_page (self->sidebar, page);

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
  if (page != self->current_page)
    nav_push_current (self);
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
    if (self->sidebar)
      fw_sidebar_set_current_page (self->sidebar, page);
  }
}

/* ── Search toggle ────────────────────────────────────────────────── */

static void
search_toggled (GtkToggleButton *button, gpointer user_data)
{
  FwWindow *self = FW_WINDOW (user_data);
  gboolean active = gtk_toggle_button_get_active (button);
  gtk_search_bar_set_search_mode (self->search_bar, active);
  if (active) {
    gtk_widget_grab_focus (GTK_WIDGET (self->search_entry));
  } else if (self->search) {
    fw_search_clear (self->search);
  }
}

/* ── Search controller wiring ───────────────────────────────────── */

static void
update_search_count_label (FwWindow *self)
{
  if (!self->search_count_label || !self->search)
    return;

  int count = fw_search_get_count (self->search);
  int current = fw_search_get_current (self->search);
  gboolean running = fw_search_is_running (self->search);

  char buf[64];
  if (count == 0)
    g_snprintf (buf, sizeof buf, "%s", running ? "Searching…" : "");
  else
    g_snprintf (buf, sizeof buf, "%d of %d%s",
                current >= 0 ? current + 1 : 0,
                count,
                running ? "+" : "");

  gtk_label_set_text (self->search_count_label, buf);
  gtk_widget_set_sensitive (GTK_WIDGET (self->search_prev_button), count > 0);
  gtk_widget_set_sensitive (GTK_WIDGET (self->search_next_button), count > 0);
}

static void
on_search_hits_changed (FwSearch *search, gpointer user_data)
{
  (void) search;
  update_search_count_label (FW_WINDOW (user_data));
}

static void
on_search_current_changed (FwSearch *search, gpointer user_data)
{
  (void) search;
  FwWindow *self = FW_WINDOW (user_data);
  update_search_count_label (self);

  /* Update header bar page entry to reflect the page that contains the
   * active hit, since the view will scroll there. */
  int page = fw_search_get_current_page (self->search);
  if (page >= 0 && page != self->current_page) {
    self->current_page = page;
    update_page_entry (self);
  }
}

static void
on_search_finished (FwSearch *search, gpointer user_data)
{
  (void) search;
  update_search_count_label (FW_WINDOW (user_data));
}

static void
on_search_entry_changed (GtkSearchEntry *entry, gpointer user_data)
{
  FwWindow *self = FW_WINDOW (user_data);
  if (!self->search)
    return;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  if (!text || !text[0]) {
    fw_search_clear (self->search);
    update_search_count_label (self);
    return;
  }
  fw_search_find (self->search, text, self->current_page);
  update_search_count_label (self);
}

static void
on_search_entry_next (GtkSearchEntry *entry, gpointer user_data)
{
  (void) entry;
  FwWindow *self = FW_WINDOW (user_data);
  if (self->search)
    fw_search_next (self->search);
}

static void
on_search_entry_previous (GtkSearchEntry *entry, gpointer user_data)
{
  (void) entry;
  FwWindow *self = FW_WINDOW (user_data);
  if (self->search)
    fw_search_prev (self->search);
}

static void
on_search_entry_stop (GtkSearchEntry *entry, gpointer user_data)
{
  (void) entry;
  FwWindow *self = FW_WINDOW (user_data);
  gtk_toggle_button_set_active (self->search_toggle, FALSE);
}

static void
search_prev_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FwWindow *self = FW_WINDOW (user_data);
  if (self->search)
    fw_search_prev (self->search);
}

static void
search_next_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FwWindow *self = FW_WINDOW (user_data);
  if (self->search)
    fw_search_next (self->search);
}

static void act_find_next (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  if (w->search)
    fw_search_next (w->search);
}

static void act_find_prev (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  if (w->search)
    fw_search_prev (w->search);
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
  if (self->document && page >= 0) {
    if (page != self->current_page)
      nav_push_current (self);
    go_to_page (self, page);
  }
}

/* ── Navigation history ───────────────────────────────────────────── */

/* Capture the current viewport as a NavEntry. */
static NavEntry
nav_capture (FwWindow *self)
{
  NavEntry e = { .page = self->current_page, .fraction = 0 };
  if (!self->view || !self->scroll || !self->document)
    return e;

  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scroll);
  if (!vadj)
    return e;

  /* Resolve current page + intra-page fraction by querying the view's
   * layout via fw_view_get_current_page() and the y-offset arithmetic
   * the rest of fw_window does. The simplest portable approach: use
   * scroll fraction within total (close enough for restore). */
  int page = fw_view_get_current_page (self->view);
  e.page = page;

  /* Fraction = (scroll_y - page_top) / page_height — but the view owns
   * those arrays. Approximate via global scroll fraction; on jump we
   * land back on the same page anyway, which is the load-bearing part. */
  double upper = gtk_adjustment_get_upper (vadj);
  double size  = gtk_adjustment_get_page_size (vadj);
  double value = gtk_adjustment_get_value (vadj);
  if (upper > size)
    e.fraction = value / (upper - size);
  return e;
}

static void
nav_apply (FwWindow *self, NavEntry e)
{
  if (!self->view)
    return;

  self->nav_in_progress = TRUE;
  fw_view_go_to_page (self->view, e.page);
  self->current_page = e.page;
  update_page_entry (self);
  if (self->sidebar)
    fw_sidebar_set_current_page (self->sidebar, e.page);

  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scroll);
  if (vadj && e.fraction > 0) {
    double upper = gtk_adjustment_get_upper (vadj);
    double size  = gtk_adjustment_get_page_size (vadj);
    if (upper > size)
      gtk_adjustment_set_value (vadj, e.fraction * (upper - size));
  }
  self->nav_in_progress = FALSE;
}

/* Push the current viewport onto the back stack. Clears the forward
 * stack — the standard browser history rule. */
static void
nav_push_current (FwWindow *self)
{
  if (!self->document || self->nav_in_progress)
    return;

  NavEntry e = nav_capture (self);
  g_array_append_val (self->nav_back, e);
  g_array_set_size (self->nav_forward, 0);
}

static void
on_view_page_jumped (FwView *view, int dest_page, gpointer user_data)
{
  (void) view; (void) dest_page;
  nav_push_current (FW_WINDOW (user_data));
}

static void act_nav_back (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  if (w->nav_back->len == 0)
    return;
  /* Push current → forward, then pop back → current. */
  NavEntry cur = nav_capture (w);
  g_array_append_val (w->nav_forward, cur);

  NavEntry e = g_array_index (w->nav_back, NavEntry, w->nav_back->len - 1);
  g_array_set_size (w->nav_back, w->nav_back->len - 1);
  nav_apply (w, e);
}

static void act_nav_forward (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  if (w->nav_forward->len == 0)
    return;
  NavEntry cur = nav_capture (w);
  g_array_append_val (w->nav_back, cur);

  NavEntry e = g_array_index (w->nav_forward, NavEntry,
                              w->nav_forward->len - 1);
  g_array_set_size (w->nav_forward, w->nav_forward->len - 1);
  nav_apply (w, e);
}

/* ── Window actions ───────────────────────────────────────────────── */

static void act_zoom_in    (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; set_zoom(w, w->zoom + 0.1); }
static void act_zoom_out   (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; set_zoom(w, w->zoom - 0.1); }
static void act_zoom_actual(GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; set_zoom(d, 1.0); }
static void act_zoom_fit_w (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; set_zoom(w, fw_view_fit_width_zoom(w->view, gtk_widget_get_width(GTK_WIDGET(w->scroll)))); }
static void act_zoom_fit_p (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; FwWindow *w=d; set_zoom(w, fw_view_fit_page_zoom(w->view, gtk_widget_get_width(GTK_WIDGET(w->scroll)), gtk_widget_get_height(GTK_WIDGET(w->scroll)))); }
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

static void
set_rotation (FwWindow *self, int rotation)
{
  rotation = ((rotation % 360) + 360) % 360;
  FW_TRACE_WINDOW ("set_rotation: %d", rotation);
  self->rotation = rotation;

  if (self->cache)
    fw_cache_start (self->cache, self->zoom, self->rotation);

  if (self->view)
    fw_view_set_rotation (self->view, rotation);
}

static void act_rotate_cw (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  set_rotation (w, w->rotation + 90);
}

static void act_rotate_ccw (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  set_rotation (w, w->rotation - 90);
}

/* ── Embedded files extraction ──────────────────────────────────── */

typedef struct {
  FwWindow *window;        /* weak: not refed; we live inside the window */
  GArray   *attachments;   /* owned */
} ExtractCtx;

static void
extract_ctx_free (ExtractCtx *ctx)
{
  if (ctx->attachments)
    g_array_unref (ctx->attachments);
  g_free (ctx);
}

static void
extract_folder_chosen (GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
  ExtractCtx *ctx = user_data;
  g_autoptr (GFile) folder =
    gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source),
                                          result, NULL);
  if (!folder) {
    extract_ctx_free (ctx);
    return;
  }

  g_autofree char *folder_path = g_file_get_path (folder);
  if (!folder_path || !ctx->window->document) {
    extract_ctx_free (ctx);
    return;
  }

  int saved = 0, failed = 0;
  GString *errors = g_string_new (NULL);

  for (guint i = 0; i < ctx->attachments->len; i++) {
    FwAttachment *a = g_array_index (ctx->attachments, FwAttachment *, i);
    g_autofree char *out_path = g_build_filename (folder_path, a->name, NULL);

    g_autoptr (GError) err = NULL;
    if (fw_document_save_attachment (ctx->window->document, a,
                                      out_path, &err)) {
      saved++;
    } else {
      failed++;
      if (err && errors->len < 1024)
        g_string_append_printf (errors, "\n%s: %s", a->name, err->message);
    }
  }

  /* Toast-style summary via AdwAlertDialog. */
  g_autofree char *heading = g_strdup_printf (
    "Extracted %d file%s%s",
    saved, saved == 1 ? "" : "s",
    failed > 0 ? " (some failed)" : "");
  g_autofree char *body = failed > 0
    ? g_strdup_printf ("%d failed:%s", failed, errors->str)
    : g_strdup_printf ("Saved to %s", folder_path);

  AdwAlertDialog *dlg = ADW_ALERT_DIALOG (
    adw_alert_dialog_new (heading, body));
  adw_alert_dialog_add_response (dlg, "ok", "OK");
  adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (ctx->window));

  g_string_free (errors, TRUE);
  extract_ctx_free (ctx);
}

static void act_save_attachments (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *self = d;
  if (!self->document)
    return;

  GArray *list = fw_document_get_attachments (self->document);
  if (!list || list->len == 0) {
    if (list) g_array_unref (list);
    AdwAlertDialog *dlg = ADW_ALERT_DIALOG (adw_alert_dialog_new (
      "No Embedded Files",
      "This document has no attached files to extract."));
    adw_alert_dialog_add_response (dlg, "ok", "OK");
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
    return;
  }

  ExtractCtx *ctx = g_new0 (ExtractCtx, 1);
  ctx->window      = self;
  ctx->attachments = list;

  GtkFileDialog *dialog = gtk_file_dialog_new ();
  g_autofree char *title = g_strdup_printf (
    "Extract %u file%s to folder…", list->len, list->len == 1 ? "" : "s");
  gtk_file_dialog_set_title (dialog, title);
  gtk_file_dialog_select_folder (dialog, GTK_WINDOW (self), NULL,
                                 extract_folder_chosen, ctx);
  g_object_unref (dialog);
}

/* ── Printing ───────────────────────────────────────────────────── */

/* Render quality: cap at 300 DPI to keep memory bounded. A US-letter page
 * at 300 DPI is 8.5*300 × 11*300 × 4 bytes = ~33 MB — acceptable for a
 * print-time render. The printer's actual native DPI may be 600/1200, but
 * cairo handles the upscale on the destination side. */
#define PRINT_DPI_CAP 300.0

static void
on_print_begin (GtkPrintOperation *op, GtkPrintContext *ctx,
                gpointer user_data)
{
  (void) ctx;
  FwWindow *self = FW_WINDOW (user_data);
  if (!self->document) {
    gtk_print_operation_set_n_pages (op, 0);
    return;
  }
  gtk_print_operation_set_n_pages (op, fw_document_get_page_count (self->document));
}

static void
on_print_draw_page (GtkPrintOperation *op, GtkPrintContext *ctx,
                    int page_nr, gpointer user_data)
{
  (void) op;
  FwWindow *self = FW_WINDOW (user_data);
  if (!self->document)
    return;

  cairo_t *cr = gtk_print_context_get_cairo_context (ctx);
  if (!cr)
    return;

  /* Render at the print context's DPI capped at 300 — same surface format
   * the cache uses, so we get the v0.7 zero-copy MuPDF render path. */
  double dpi = gtk_print_context_get_dpi_x (ctx);
  if (dpi <= 0)        dpi = 72.0;
  if (dpi > PRINT_DPI_CAP) dpi = PRINT_DPI_CAP;
  double zoom = dpi / 72.0;

  cairo_surface_t *surface =
    fw_document_render_page (self->document, page_nr, zoom, 0);
  if (!surface) {
    g_warning ("Print: render failed for page %d", page_nr + 1);
    return;
  }

  /* Cairo print context uses points (72 DPI). Source surface is in pixels
   * at `zoom` magnification — scale down so 1 source pixel maps to 1/zoom
   * points, which is the correct on-paper size. */
  cairo_save (cr);
  cairo_scale (cr, 1.0 / zoom, 1.0 / zoom);
  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_paint (cr);
  cairo_restore (cr);

  cairo_surface_destroy (surface);

  FW_TRACE_WINDOW ("print: page %d rendered at %.0f DPI", page_nr + 1, dpi);
}

static void act_print (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *self = d;
  if (!self->document)
    return;

  GtkPrintOperation *op = gtk_print_operation_new ();
  gtk_print_operation_set_embed_page_setup (op, TRUE);
  gtk_print_operation_set_show_progress (op, TRUE);

  if (self->file_path) {
    g_autofree char *base = g_path_get_basename (self->file_path);
    gtk_print_operation_set_job_name (op, base);
  }

  g_signal_connect (op, "begin-print",
                    G_CALLBACK (on_print_begin), self);
  g_signal_connect (op, "draw-page",
                    G_CALLBACK (on_print_draw_page), self);

  GError *err = NULL;
  gtk_print_operation_run (op,
                            GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                            GTK_WINDOW (self), &err);
  if (err) {
    g_warning ("Print: %s", err->message);
    g_error_free (err);
  }
  g_object_unref (op);
}

static void act_copy (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a;(void)p;
  FwWindow *w = d;
  if (!w->view)
    return;
  const char *text = fw_view_get_selected_text (w->view);
  FW_TRACE_WINDOW ("copy: text=%s len=%d",
                    text ? "yes" : "no",
                    text ? (int) strlen (text) : 0);
  if (text && text[0]) {
    GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (w));
    GdkClipboard *clipboard = gdk_display_get_clipboard (display);
    gdk_clipboard_set_text (clipboard, text);
  }
}

/* ── Keyboard Shortcuts dialog ──────────────────────────────────── */

static void
add_shortcut_row (AdwPreferencesGroup *group, const char *title,
                  const char *accel)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);

  GtkWidget *label = gtk_shortcut_label_new (accel);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (row, label);

  adw_preferences_group_add (group, GTK_WIDGET (row));
}

static void act_shortcuts (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a; (void)p;
  FwWindow *self = d;

  AdwDialog *dlg = adw_dialog_new ();
  adw_dialog_set_title (dlg, "Keyboard Shortcuts");
  adw_dialog_set_content_width (dlg, 520);
  adw_dialog_set_content_height (dlg, 640);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar),
                                 adw_header_bar_new ());
  adw_dialog_set_child (dlg, toolbar);

  GtkWidget *page = adw_preferences_page_new ();
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), page);

  AdwPreferencesGroup *g_file =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g_file, "File");
  add_shortcut_row (g_file, "Open",  "<Control>o");
  add_shortcut_row (g_file, "Print", "<Control>p");
  add_shortcut_row (g_file, "Quit",  "<Control>q <Control>w");
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), g_file);

  AdwPreferencesGroup *g_nav =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g_nav, "Navigation");
  add_shortcut_row (g_nav, "Next page",     "Page_Down");
  add_shortcut_row (g_nav, "Previous page", "Page_Up");
  add_shortcut_row (g_nav, "First page",    "Home");
  add_shortcut_row (g_nav, "Last page",     "End");
  add_shortcut_row (g_nav, "Go to page…",   "<Control>g");
  add_shortcut_row (g_nav, "Go back",       "<Alt>Left");
  add_shortcut_row (g_nav, "Go forward",    "<Alt>Right");
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), g_nav);

  AdwPreferencesGroup *g_zoom =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g_zoom, "Zoom & Rotation");
  add_shortcut_row (g_zoom, "Zoom in",                  "<Control>plus");
  add_shortcut_row (g_zoom, "Zoom out",                 "<Control>minus");
  add_shortcut_row (g_zoom, "Actual size",              "<Control>0");
  add_shortcut_row (g_zoom, "Fit width",                "<Control>1");
  add_shortcut_row (g_zoom, "Fit page",                 "<Control>2");
  add_shortcut_row (g_zoom, "Rotate clockwise",         "<Control><Shift>plus");
  add_shortcut_row (g_zoom, "Rotate counter-clockwise", "<Control><Shift>minus");
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), g_zoom);

  AdwPreferencesGroup *g_search =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g_search, "Search");
  add_shortcut_row (g_search, "Find",          "<Control>f");
  add_shortcut_row (g_search, "Find next",     "F3");
  add_shortcut_row (g_search, "Find previous", "<Shift>F3");
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), g_search);

  AdwPreferencesGroup *g_view =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g_view, "View");
  add_shortcut_row (g_view, "Toggle sidebar",   "F9");
  add_shortcut_row (g_view, "Fullscreen",       "F11");
  add_shortcut_row (g_view, "Invert colors",    "<Control>i");
  add_shortcut_row (g_view, "Reading ruler",    "F8");
  add_shortcut_row (g_view, "Magnifying loupe", "F7");
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), g_view);

  AdwPreferencesGroup *g_sel =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g_sel, "Selection");
  add_shortcut_row (g_sel, "Copy", "<Control>c");
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), g_sel);

  adw_dialog_present (dlg, GTK_WIDGET (self));
}

/* ── Document Properties dialog ─────────────────────────────────── */

static void
add_property_row (AdwPreferencesGroup *group, const char *title,
                  const char *value)
{
  if (!value || !*value) return;
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_action_row_set_subtitle (row, value);
  adw_action_row_set_subtitle_selectable (row, TRUE);
  adw_preferences_group_add (group, GTK_WIDGET (row));
}

static void act_properties (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void)a; (void)p;
  FwWindow *self = d;
  if (!self->document) return;

  AdwDialog *dlg = adw_dialog_new ();
  adw_dialog_set_title (dlg, "Document Properties");
  adw_dialog_set_content_width (dlg, 480);
  adw_dialog_set_content_height (dlg, 560);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar),
                                 adw_header_bar_new ());
  adw_dialog_set_child (dlg, toolbar);

  GtkWidget *page = adw_preferences_page_new ();
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), page);

  GHashTable *meta = fw_document_get_metadata (self->document);

  /* Document group — backend metadata (title, author, dates, etc.) */
  AdwPreferencesGroup *doc_group =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (doc_group, "Document");
  if (meta) {
    add_property_row (doc_group, "Title",    g_hash_table_lookup (meta, "title"));
    add_property_row (doc_group, "Author",   g_hash_table_lookup (meta, "author"));
    add_property_row (doc_group, "Subject",  g_hash_table_lookup (meta, "subject"));
    add_property_row (doc_group, "Keywords", g_hash_table_lookup (meta, "keywords"));
    add_property_row (doc_group, "Creator",  g_hash_table_lookup (meta, "creator"));
    add_property_row (doc_group, "Producer", g_hash_table_lookup (meta, "producer"));
    add_property_row (doc_group, "Created",  g_hash_table_lookup (meta, "creation-date"));
    add_property_row (doc_group, "Modified", g_hash_table_lookup (meta, "modification-date"));
  }
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), doc_group);

  /* File group — derived from the open path + backend format/encryption */
  AdwPreferencesGroup *file_group =
    ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (file_group, "File");

  if (self->file_path) {
    g_autofree char *basename = g_path_get_basename (self->file_path);
    add_property_row (file_group, "Filename", basename);

    GStatBuf st;
    if (g_stat (self->file_path, &st) == 0) {
      g_autofree char *size = g_format_size ((guint64) st.st_size);
      add_property_row (file_group, "Size", size);
    }
    add_property_row (file_group, "Location", self->file_path);
  }
  if (meta) {
    add_property_row (file_group, "Format",     g_hash_table_lookup (meta, "format"));
    add_property_row (file_group, "Encryption", g_hash_table_lookup (meta, "encryption"));
  }

  g_autofree char *pages =
    g_strdup_printf ("%d", fw_document_get_page_count (self->document));
  add_property_row (file_group, "Pages", pages);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), file_group);

  if (meta) g_hash_table_unref (meta);

  adw_dialog_present (dlg, GTK_WIDGET (self));
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

#define SCROLL_STEP        50.0
/* Hard cap on single-event scroll distance. Without this, a fast wheel flick
 * (high delta) or amplified trackpad event can blow past multiple pages in
 * one frame — faster than the cache can render, producing the "stuck in
 * pre-cache" gray flashes. Capping at ~1.5 lines of text keeps scroll
 * velocity bounded so the cache can keep up. */
#define SCROLL_MAX_STEP    120.0

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

  /* Scroll damping: consume the event and apply a capped step. Bypasses
   * GTK's kinetic scrolling (which can amplify wheel events unpredictably)
   * and enforces a maximum per-event movement so the cache can keep up. */
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scroll);
  if (!vadj)
    return FALSE;

  double step = dy * SCROLL_STEP;
  if (step > SCROLL_MAX_STEP) step = SCROLL_MAX_STEP;
  if (step < -SCROLL_MAX_STEP) step = -SCROLL_MAX_STEP;

  gtk_adjustment_set_value (vadj, gtk_adjustment_get_value (vadj) + step);
  return TRUE;
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

/* ── Drag-and-drop ──────────────────────────────────────────────── */

static gboolean
on_drop (GtkDropTarget *target, const GValue *value, double x, double y,
         gpointer user_data)
{
  (void) target; (void) x; (void) y;
  FwWindow *self = FW_WINDOW (user_data);

  if (!G_VALUE_HOLDS (value, G_TYPE_FILE))
    return FALSE;

  GFile *file = g_value_get_object (value);
  if (!file)
    return FALSE;

  g_autofree char *path = g_file_get_path (file);
  if (!path)
    return FALSE;

  FW_TRACE_WINDOW ("drop: '%s'", path);
  fw_window_open_file (self, path);
  return TRUE;
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
  g_menu_append (menu, "Kinetic Scrolling", "win.kinetic-scrolling");
  g_menu_append (menu, "Reading Ruler", "win.reading-ruler");
  g_menu_append (menu, "Magnifying Loupe", "win.loupe");
  g_menu_append (menu, "Print…", "win.print");
  g_menu_append (menu, "Save Embedded Files…", "win.save-attachments");
  g_menu_append (menu, "Document Properties…", "win.properties");
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
  g_signal_connect (self->view, "page-jumped",
                    G_CALLBACK (on_view_page_jumped), self);
  self->scroll = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_child (self->scroll, GTK_WIDGET (self->view));
  gtk_scrolled_window_set_policy (self->scroll,
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
  /* kinetic-scrolling reflected from GSettings further below
   * (after self->settings is allocated). */

  /* Track current page as user scrolls */
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scroll);
  if (vadj)
    g_signal_connect (vadj, "value-changed",
                      G_CALLBACK (on_scroll_changed), self);
  gtk_widget_set_vexpand (GTK_WIDGET (self->scroll), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->scroll), TRUE);

  /* Search bar overlay */
  self->search_entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (self->search_entry), TRUE);
  g_signal_connect (self->search_entry, "search-changed",
                    G_CALLBACK (on_search_entry_changed), self);
  g_signal_connect (self->search_entry, "next-match",
                    G_CALLBACK (on_search_entry_next), self);
  g_signal_connect (self->search_entry, "previous-match",
                    G_CALLBACK (on_search_entry_previous), self);
  g_signal_connect (self->search_entry, "stop-search",
                    G_CALLBACK (on_search_entry_stop), self);

  self->search_count_label = GTK_LABEL (gtk_label_new (""));
  gtk_widget_add_css_class (GTK_WIDGET (self->search_count_label), "dim-label");
  gtk_widget_set_size_request (GTK_WIDGET (self->search_count_label), 90, -1);
  gtk_label_set_xalign (self->search_count_label, 0.5);

  self->search_prev_button = GTK_BUTTON (
    gtk_button_new_from_icon_name ("go-up-symbolic"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->search_prev_button),
                               "Previous Match (Shift+F3)");
  gtk_widget_set_sensitive (GTK_WIDGET (self->search_prev_button), FALSE);
  g_signal_connect (self->search_prev_button, "clicked",
                    G_CALLBACK (search_prev_clicked), self);

  self->search_next_button = GTK_BUTTON (
    gtk_button_new_from_icon_name ("go-down-symbolic"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->search_next_button),
                               "Next Match (F3)");
  gtk_widget_set_sensitive (GTK_WIDGET (self->search_next_button), FALSE);
  g_signal_connect (self->search_next_button, "clicked",
                    G_CALLBACK (search_next_clicked), self);

  GtkBox *search_row = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6));
  gtk_box_append (search_row, GTK_WIDGET (self->search_entry));
  gtk_box_append (search_row, GTK_WIDGET (self->search_count_label));
  gtk_box_append (search_row, GTK_WIDGET (self->search_prev_button));
  gtk_box_append (search_row, GTK_WIDGET (self->search_next_button));

  self->search_bar = GTK_SEARCH_BAR (gtk_search_bar_new ());
  gtk_search_bar_set_child (self->search_bar, GTK_WIDGET (search_row));
  gtk_search_bar_connect_entry (self->search_bar,
                                GTK_EDITABLE (self->search_entry));
  gtk_widget_set_valign (GTK_WIDGET (self->search_bar), GTK_ALIGN_START);

  /* Search controller — created here, populated when a doc opens. */
  self->search = fw_search_new ();
  g_signal_connect (self->search, "hits-changed",
                    G_CALLBACK (on_search_hits_changed), self);
  g_signal_connect (self->search, "current-changed",
                    G_CALLBACK (on_search_current_changed), self);
  g_signal_connect (self->search, "search-finished",
                    G_CALLBACK (on_search_finished), self);

  GtkOverlay *overlay = GTK_OVERLAY (gtk_overlay_new ());
  gtk_overlay_set_child (overlay, GTK_WIDGET (self->scroll));
  gtk_overlay_add_overlay (overlay, GTK_WIDGET (self->search_bar));
  gtk_widget_set_vexpand (GTK_WIDGET (overlay), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (overlay), TRUE);

  /* Empty-state page shown when no document is open. */
  AdwStatusPage *empty = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (empty, APP_ID);
  adw_status_page_set_title (empty, "Open a Document");
  adw_status_page_set_description (empty,
    "Drop a PDF, DjVu, or CBZ file here, or use Ctrl+O.");
  GtkButton *empty_btn = GTK_BUTTON (gtk_button_new_with_label ("Open File…"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (empty_btn), "app.open");
  gtk_widget_set_halign (GTK_WIDGET (empty_btn), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (empty_btn), "suggested-action");
  gtk_widget_add_css_class (GTK_WIDGET (empty_btn), "pill");
  adw_status_page_set_child (empty, GTK_WIDGET (empty_btn));

  /* Stack flips between the empty state and the rendered document. */
  self->content_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->content_stack,
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named (self->content_stack, GTK_WIDGET (empty), "empty");
  gtk_stack_add_named (self->content_stack, GTK_WIDGET (overlay), "document");
  gtk_stack_set_visible_child_name (self->content_stack, "empty");
  gtk_widget_set_vexpand (GTK_WIDGET (self->content_stack), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->content_stack), TRUE);

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
                                       GTK_WIDGET (self->content_stack));
  adw_overlay_split_view_set_show_sidebar (self->split_view, FALSE);
  adw_overlay_split_view_set_max_sidebar_width (self->split_view, 280);
  gtk_widget_set_vexpand (GTK_WIDGET (self->split_view), TRUE);

  /* ── Main box ── */
  GtkBox *box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_append (box, GTK_WIDGET (self->header_bar));
  gtk_box_append (box, GTK_WIDGET (self->split_view));

  /* Toast overlay wraps the entire content so AdwToasts (e.g.,
   * "Document updated" on auto-reload) float above everything else. */
  self->toast_overlay = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toast_overlay, GTK_WIDGET (box));

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                       GTK_WIDGET (self->toast_overlay));

  /* Initialize state */
  self->zoom = 1.0;
  self->rotation = 0;
  self->current_page = 0;
  self->nav_back    = g_array_new (FALSE, FALSE, sizeof (NavEntry));
  self->nav_forward = g_array_new (FALSE, FALSE, sizeof (NavEntry));
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
    { .name = "find-next",     .activate = act_find_next },
    { .name = "find-prev",     .activate = act_find_prev },
    { .name = "nav-back",      .activate = act_nav_back },
    { .name = "nav-forward",   .activate = act_nav_forward },
    { .name = "go-to-page",    .activate = act_go_to_page },
    { .name = "invert-colors", .activate = act_invert_colors },
    { .name = "rotate-cw",     .activate = act_rotate_cw },
    { .name = "rotate-ccw",    .activate = act_rotate_ccw },
    { .name = "copy",          .activate = act_copy },
    { .name = "print",         .activate = act_print },
    { .name = "save-attachments",  .activate = act_save_attachments },
    { .name = "properties",        .activate = act_properties },
    { .name = "show-help-overlay", .activate = act_shortcuts },
    { .name = "about",             .activate = act_about },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (self), win_entries,
                                   G_N_ELEMENTS (win_entries), self);

  /* ── Stateful settings-backed action: Kinetic Scrolling toggle ──
   * g_settings_create_action returns a GAction whose state is bound
   * to the GSettings key — the menu checkmark stays in sync, and
   * activating the action flips the setting (which fw-view listens to). */
  {
    /* Long-lived settings handle on the window — used both for the
     * stateful actions wired into the menu *and* for the
     * kinetic-scrolling change signal that drives the scrolled
     * window. */
    self->settings = g_settings_new (APP_ID);

    g_autoptr (GAction) kinetic_action =
      g_settings_create_action (self->settings, "kinetic-scrolling");
    g_action_map_add_action (G_ACTION_MAP (self), kinetic_action);

    g_autoptr (GAction) ruler_action =
      g_settings_create_action (self->settings, "reading-ruler");
    g_action_map_add_action (G_ACTION_MAP (self), ruler_action);

    g_autoptr (GAction) loupe_action =
      g_settings_create_action (self->settings, "loupe");
    g_action_map_add_action (G_ACTION_MAP (self), loupe_action);

    /* Apply kinetic-scrolling to the scrolled window now and on every
     * change. The previous hardcoded TRUE is now driven by the user's
     * setting. */
    gtk_scrolled_window_set_kinetic_scrolling (
      self->scroll,
      g_settings_get_boolean (self->settings, "kinetic-scrolling"));
    self->settings_kinetic_handler = g_signal_connect (
      self->settings, "changed::kinetic-scrolling",
      G_CALLBACK (on_kinetic_setting_changed), self);
  }

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

  /* Drag-and-drop: drop a file onto the window to open it */
  GtkDropTarget *drop = gtk_drop_target_new (G_TYPE_FILE, GDK_ACTION_COPY);
  g_signal_connect (drop, "drop", G_CALLBACK (on_drop), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drop));
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

/* ── Settings change handlers ────────────────────────────────────── */

static void
on_kinetic_setting_changed (GSettings *settings, const char *key,
                            gpointer user_data)
{
  (void) key;
  FwWindow *self = FW_WINDOW (user_data);
  if (!self->scroll)
    return;
  gboolean kinetic = g_settings_get_boolean (settings, "kinetic-scrolling");
  gtk_scrolled_window_set_kinetic_scrolling (self->scroll, kinetic);
  FW_TRACE_WINDOW ("kinetic-scrolling=%d", kinetic);
}

/* ── Auto-reload via GFileMonitor ────────────────────────────────── */

static void
fw_window_show_toast (FwWindow *self, const char *text)
{
  if (!self->toast_overlay)
    return;
  AdwToast *toast = adw_toast_new (text);
  adw_toast_set_timeout (toast, 2);
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

static gboolean
reload_document_idle (gpointer user_data)
{
  FwWindow *self = FW_WINDOW (user_data);
  self->reload_debounce_id = 0;

  if (!self->file_path)
    return G_SOURCE_REMOVE;

  /* fw_window_open_file frees self->file_path early — copy first. */
  g_autofree char *path = g_strdup (self->file_path);
  FW_TRACE_WINDOW ("auto-reload: '%s'", path);
  fw_window_open_file (self, path);
  fw_window_show_toast (self, "Document updated");
  return G_SOURCE_REMOVE;
}

static void
on_file_monitor_changed (GFileMonitor      *monitor,
                         GFile             *file,
                         GFile             *other_file,
                         GFileMonitorEvent  event,
                         gpointer           user_data)
{
  (void) monitor; (void) file; (void) other_file;
  FwWindow *self = FW_WINDOW (user_data);

  /* CHANGES_DONE_HINT fires when the writer finishes (after the inotify
   * close-write equivalent). CREATED covers atomic-rename editors that
   * unlink+create rather than write in place. Both events trigger a
   * debounced reload — multiple events in quick succession (LaTeX often
   * writes auxiliary files in the same directory) collapse to a single
   * reload that runs after a 200 ms quiet window. */
  if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
      event != G_FILE_MONITOR_EVENT_CREATED)
    return;

  if (self->reload_debounce_id)
    g_source_remove (self->reload_debounce_id);
  self->reload_debounce_id = g_timeout_add (200, reload_document_idle, self);
}

static void
fw_window_stop_monitor (FwWindow *self)
{
  if (self->reload_debounce_id) {
    g_source_remove (self->reload_debounce_id);
    self->reload_debounce_id = 0;
  }
  if (self->file_monitor) {
    g_signal_handlers_disconnect_by_func (self->file_monitor,
                                          on_file_monitor_changed, self);
    g_file_monitor_cancel (self->file_monitor);
    g_clear_object (&self->file_monitor);
  }
}

static void
fw_window_start_monitor (FwWindow *self, const char *path)
{
  if (!path)
    return;
  g_autoptr (GFile) gfile = g_file_new_for_path (path);
  g_autoptr (GError) error = NULL;
  /* WATCH_HARD_LINKS makes inotify track the inode, so atomic-rename
   * patterns (write-to-tmp, rename-over) still produce events. */
  GFileMonitor *m = g_file_monitor_file (gfile,
                                          G_FILE_MONITOR_WATCH_HARD_LINKS,
                                          NULL, &error);
  if (!m) {
    g_warning ("auto-reload: could not watch '%s': %s",
               path, error ? error->message : "(null)");
    return;
  }
  self->file_monitor = m;
  g_signal_connect (m, "changed",
                    G_CALLBACK (on_file_monitor_changed), self);
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

  /* New document → clear navigation history */
  if (self->nav_back)    g_array_set_size (self->nav_back, 0);
  if (self->nav_forward) g_array_set_size (self->nav_forward, 0);

  /* Stop watching the old file before tearing it down — avoids spurious
   * change events firing on the path we're about to swap. */
  fw_window_stop_monitor (self);

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

  /* Start watching the new file for changes — auto-reload on
   * recompile (LaTeX, Typst, anything that rewrites the same path). */
  fw_window_start_monitor (self, self->file_path);

  /* Switch from empty state to the document view. */
  if (self->content_stack)
    gtk_stack_set_visible_child_name (self->content_stack, "document");

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

  /* Hand the new document to search; clear any prior query. */
  fw_search_set_document (self->search, self->document);
  fw_view_set_search (self->view, self->search);
  if (self->search_entry)
    gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
  update_search_count_label (self);

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
  fw_window_stop_monitor (self);

  if (self->settings) {
    if (self->settings_kinetic_handler)
      g_signal_handler_disconnect (self->settings, self->settings_kinetic_handler);
    self->settings_kinetic_handler = 0;
    g_clear_object (&self->settings);
  }

  /* Disconnect view from document/cache FIRST — the view holds refs to both,
   * and GTK widget teardown order is unpredictable. Without this, the cache
   * refcount never reaches zero during window dispose. */
  if (self->view) {
    fw_view_set_search (self->view, NULL);
    fw_view_set_document (self->view, NULL, NULL);
  }
  g_clear_object (&self->search);

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
  g_clear_pointer (&self->nav_back,    g_array_unref);
  g_clear_pointer (&self->nav_forward, g_array_unref);

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
