/* fw-view.c — Custom page rendering widget
 *
 * Implements GtkScrollable for proper integration with GtkScrolledWindow.
 * Lays out rendered pages vertically in continuous scroll mode.
 * Only paints pages visible in the current viewport.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-view.h"
#include "fw-debug.h"

#define PAGE_GAP 8

struct _FwView {
  GtkWidget    parent_instance;

  FwDocument  *document;
  FwCache     *cache;

  double       zoom;
  int          rotation;    /* 0, 90, 180, 270 */
  int          page_count;

  /* Cached page sizes at current zoom (in pixels) */
  double      *page_widths;
  double      *page_heights;
  double      *page_y_offsets;   /* cumulative y offset per page */
  double       total_height;
  double       max_width;

  /* GtkScrollable */
  GtkAdjustment *hadjustment;
  GtkAdjustment *vadjustment;
  GtkScrollablePolicy hscroll_policy;
  GtkScrollablePolicy vscroll_policy;

  /* Velocity tracking */
  double       last_scroll_y;
  gint64       last_frame_time;
  double       smooth_velocity;

  /* Display options */
  gboolean     invert_colors;
  gboolean     redraw_pending;

  /* Text selection */
  int          sel_page;       /* -1 = no selection */
  double       sel_x0, sel_y0; /* start in document points */
  double       sel_x1, sel_y1; /* end in document points */
  gboolean     selecting;
  char        *selected_text;

  /* Link cache — lazily populated per page */
  GArray     **link_cache;     /* array of GArray* indexed by page, or NULL */
  int          link_cache_count;
};

static void fw_view_scrollable_init (GtkScrollableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwView, fw_view, GTK_TYPE_WIDGET,
  G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, fw_view_scrollable_init))

enum {
  PROP_0,
  PROP_HADJUSTMENT,
  PROP_VADJUSTMENT,
  PROP_HSCROLL_POLICY,
  PROP_VSCROLL_POLICY,
  N_PROPS,
};

/* ── Adjustment handling ──────────────────────────────────────────── */

static void
update_cache_priority (FwView *self)
{
  if (!self->cache || !self->vadjustment || !self->page_y_offsets)
    return;

  int widget_height = gtk_widget_get_height (GTK_WIDGET (self));
  if (widget_height <= 0)
    return;

  double scroll_y   = gtk_adjustment_get_value (self->vadjustment);
  double vis_top    = scroll_y;
  double vis_bottom = scroll_y + widget_height;

  /* Collect visible page indices */
  int visible[64];
  int n_visible = 0;

  for (int i = 0; i < self->page_count && n_visible < 64; i++) {
    double py = self->page_y_offsets[i];
    double ph = self->page_heights[i];
    if (py + ph < vis_top) continue;
    if (py > vis_bottom)   break;
    visible[n_visible++] = i;
  }

  if (n_visible > 0)
    fw_cache_set_priority (self->cache, visible, n_visible);
}

static void
adjustment_value_changed (GtkAdjustment *adj, gpointer user_data)
{
  (void) adj;
  FwView *self = FW_VIEW (user_data);
  update_cache_priority (self);
  if (!self->redraw_pending) {
    self->redraw_pending = TRUE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

static void
fw_view_set_hadjustment (FwView *self, GtkAdjustment *adj)
{
  if (self->hadjustment == adj)
    return;

  if (self->hadjustment)
    g_signal_handlers_disconnect_by_func (self->hadjustment,
                                          adjustment_value_changed, self);

  g_set_object (&self->hadjustment, adj);

  if (self->hadjustment)
    g_signal_connect (self->hadjustment, "value-changed",
                      G_CALLBACK (adjustment_value_changed), self);
}

static void
fw_view_set_vadjustment (FwView *self, GtkAdjustment *adj)
{
  if (self->vadjustment == adj)
    return;

  if (self->vadjustment)
    g_signal_handlers_disconnect_by_func (self->vadjustment,
                                          adjustment_value_changed, self);

  g_set_object (&self->vadjustment, adj);

  if (self->vadjustment)
    g_signal_connect (self->vadjustment, "value-changed",
                      G_CALLBACK (adjustment_value_changed), self);
}

static void
update_adjustments (FwView *self)
{
  int alloc_w = gtk_widget_get_width (GTK_WIDGET (self));
  int alloc_h = gtk_widget_get_height (GTK_WIDGET (self));

  if (self->hadjustment) {
    double content_w = self->max_width;
    if (content_w < alloc_w) content_w = alloc_w;
    gtk_adjustment_configure (self->hadjustment,
                              gtk_adjustment_get_value (self->hadjustment),
                              0, content_w, alloc_w * 0.1, alloc_w * 0.9,
                              alloc_w);
  }

  if (self->vadjustment) {
    double content_h = self->total_height;
    if (content_h < alloc_h) content_h = alloc_h;
    double old_val = gtk_adjustment_get_value (self->vadjustment);
    if (old_val > content_h - alloc_h)
      old_val = content_h - alloc_h;
    if (old_val < 0) old_val = 0;
    gtk_adjustment_configure (self->vadjustment,
                              old_val,
                              0, content_h, alloc_h * 0.1, alloc_h * 0.9,
                              alloc_h);
  }
}

/* ── Recompute layout ─────────────────────────────────────────────── */

static void
recompute_layout (FwView *self)
{
  if (!self->document || self->page_count == 0)
    return;

  g_free (self->page_widths);
  g_free (self->page_heights);
  g_free (self->page_y_offsets);

  self->page_widths    = g_new (double, self->page_count);
  self->page_heights   = g_new (double, self->page_count);
  self->page_y_offsets = g_new (double, self->page_count);
  self->total_height = 0;
  self->max_width    = 0;

  for (int i = 0; i < self->page_count; i++) {
    double w, h;
    fw_document_get_page_size (self->document, i, &w, &h);

    /* Swap dimensions for 90/270 degree rotation */
    if (self->rotation == 90 || self->rotation == 270) {
      double tmp = w;
      w = h;
      h = tmp;
    }

    self->page_widths[i]  = w * self->zoom;
    self->page_heights[i] = h * self->zoom;
    self->page_y_offsets[i] = self->total_height;

    if (self->page_widths[i] > self->max_width)
      self->max_width = self->page_widths[i];

    self->total_height += self->page_heights[i];
    if (i < self->page_count - 1)
      self->total_height += PAGE_GAP;
  }

  update_adjustments (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ── GtkWidget vfuncs ─────────────────────────────────────────────── */

static void
fw_view_measure (GtkWidget      *widget,
                 GtkOrientation  orientation,
                 int             for_size,
                 int            *minimum,
                 int            *natural,
                 int            *minimum_baseline,
                 int            *natural_baseline)
{
  (void) for_size;
  FwView *self = FW_VIEW (widget);

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = 100;
    *natural = (int) self->max_width;
  } else {
    /* As a scrollable widget, report minimum = 0 so the scrolled window
     * can make us smaller than the content. */
    *minimum = 0;
    *natural = (int) self->total_height;
  }

  *minimum_baseline = -1;
  *natural_baseline = -1;
}

static void
fw_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  (void) baseline;
  (void) width;
  (void) height;
  FwView *self = FW_VIEW (widget);
  update_adjustments (self);
}

static void
fw_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  FwView *self = FW_VIEW (widget);
  self->redraw_pending = FALSE;

  if (!self->document || !self->cache || !self->page_y_offsets)
    return;

  int widget_width  = gtk_widget_get_width (widget);
  int widget_height = gtk_widget_get_height (widget);

  double scroll_x = self->hadjustment
    ? gtk_adjustment_get_value (self->hadjustment) : 0;
  double scroll_y = self->vadjustment
    ? gtk_adjustment_get_value (self->vadjustment) : 0;

  /* Visible range */
  double vis_top    = scroll_y;
  double vis_bottom = scroll_y + widget_height;

  for (int i = 0; i < self->page_count; i++) {
    double pw = self->page_widths[i];
    double ph = self->page_heights[i];
    double py = self->page_y_offsets[i];

    /* Skip pages above/below viewport */
    if (py + ph < vis_top)
      continue;
    if (py > vis_bottom)
      break;

    /* Position relative to viewport — center if narrower than viewport,
     * otherwise offset by horizontal scroll */
    double x;
    if (self->max_width <= widget_width)
      x = (widget_width - pw) / 2.0;
    else
      x = (self->max_width - pw) / 2.0 - scroll_x;
    double y = py - scroll_y;

    graphene_rect_t rect = GRAPHENE_RECT_INIT ((float) x, (float) y,
                                                (float) pw, (float) ph);

    /* Textures are cached in CacheEntry and reused across frames —
     * zero per-frame allocation. fw_cache_get_texture also falls back
     * to the prev-gen texture for zoom transitions. */
    GdkTexture *texture = fw_cache_get_texture (self->cache, i);

    if (!texture) {
      /* No full-res texture yet. Request a thumbnail — renders lazily in
       * the background. If available, use it as a scaled placeholder. */
      double doc_w, doc_h;
      fw_document_get_page_size (self->document, i, &doc_w, &doc_h);
      if (self->rotation == 90 || self->rotation == 270) {
        double tmp = doc_w; doc_w = doc_h; doc_h = tmp;
      }
      texture = fw_cache_get_thumbnail (self->cache, i, doc_w, doc_h);
    }

    if (texture) {
      if (self->invert_colors) {
        graphene_matrix_t color_matrix;
        graphene_matrix_init_from_float (&color_matrix,
          (const float[16]) {
            -1,  0,  0,  0,
             0, -1,  0,  0,
             0,  0, -1,  0,
             0,  0,  0,  1,
          });
        graphene_vec4_t color_offset;
        graphene_vec4_init (&color_offset, 1.0f, 1.0f, 1.0f, 0.0f);
        gtk_snapshot_push_color_matrix (snapshot, &color_matrix, &color_offset);
      }

      gtk_snapshot_append_texture (snapshot, texture, &rect);

      if (self->invert_colors)
        gtk_snapshot_pop (snapshot);
      /* texture is borrowed from the cache — do not unref */
    } else {
      GdkRGBA gray = { 0.92f, 0.92f, 0.92f, 1.0f };
      gtk_snapshot_append_color (snapshot, &gray, &rect);
    }

    /* Paint text selection overlay on the selected page */
    if (self->sel_page == i &&
        (self->selecting || self->selected_text)) {
      double sx0 = self->sel_x0 < self->sel_x1 ? self->sel_x0 : self->sel_x1;
      double sy0 = self->sel_y0 < self->sel_y1 ? self->sel_y0 : self->sel_y1;
      double sx1 = self->sel_x0 > self->sel_x1 ? self->sel_x0 : self->sel_x1;
      double sy1 = self->sel_y0 > self->sel_y1 ? self->sel_y0 : self->sel_y1;

      /* Convert from document points to widget coordinates */
      float sel_wx = (float) (x + sx0 * self->zoom);
      float sel_wy = (float) (y + sy0 * self->zoom);
      float sel_ww = (float) ((sx1 - sx0) * self->zoom);
      float sel_wh = (float) ((sy1 - sy0) * self->zoom);

      if (sel_ww > 0 && sel_wh > 0) {
        graphene_rect_t sel_rect = GRAPHENE_RECT_INIT (
          sel_wx, sel_wy, sel_ww, sel_wh);
        GdkRGBA sel_color = { 0.2f, 0.4f, 0.8f, 0.3f };
        gtk_snapshot_append_color (snapshot, &sel_color, &sel_rect);
      }
    }
  }
}

/* ── GtkScrollable interface ──────────────────────────────────────── */

static void
fw_view_scrollable_init (GtkScrollableInterface *iface)
{
  /* Default implementations from GtkScrollable are sufficient —
   * we just need the interface and the properties. */
  (void) iface;
}

/* ── Properties ───────────────────────────────────────────────────── */

static void
fw_view_set_property (GObject *object, guint prop_id,
                      const GValue *value, GParamSpec *pspec)
{
  FwView *self = FW_VIEW (object);

  switch (prop_id) {
  case PROP_HADJUSTMENT:
    fw_view_set_hadjustment (self, g_value_get_object (value));
    break;
  case PROP_VADJUSTMENT:
    fw_view_set_vadjustment (self, g_value_get_object (value));
    break;
  case PROP_HSCROLL_POLICY:
    self->hscroll_policy = g_value_get_enum (value);
    break;
  case PROP_VSCROLL_POLICY:
    self->vscroll_policy = g_value_get_enum (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
fw_view_get_property (GObject *object, guint prop_id,
                      GValue *value, GParamSpec *pspec)
{
  FwView *self = FW_VIEW (object);

  switch (prop_id) {
  case PROP_HADJUSTMENT:
    g_value_set_object (value, self->hadjustment);
    break;
  case PROP_VADJUSTMENT:
    g_value_set_object (value, self->vadjustment);
    break;
  case PROP_HSCROLL_POLICY:
    g_value_set_enum (value, self->hscroll_policy);
    break;
  case PROP_VSCROLL_POLICY:
    g_value_set_enum (value, self->vscroll_policy);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

/* ── Coordinate mapping ──────────────────────────────────────────── */

/* Map widget coordinates (e.g., from a gesture) to document page coordinates.
 * Returns TRUE if the point is over a page, with page index and doc coords. */
static gboolean
fw_view_widget_to_doc (FwView *self, double widget_x, double widget_y,
                       int *out_page, double *out_doc_x, double *out_doc_y)
{
  if (!self->vadjustment || !self->page_y_offsets || self->page_count == 0)
    return FALSE;

  double scroll_x = self->hadjustment
    ? gtk_adjustment_get_value (self->hadjustment) : 0;
  double scroll_y = self->vadjustment
    ? gtk_adjustment_get_value (self->vadjustment) : 0;

  int widget_width = gtk_widget_get_width (GTK_WIDGET (self));
  double abs_y = widget_y + scroll_y;

  for (int i = 0; i < self->page_count; i++) {
    double py = self->page_y_offsets[i];
    double pw = self->page_widths[i];
    double ph = self->page_heights[i];

    if (abs_y < py || abs_y > py + ph)
      continue;

    /* Compute page X position (mirrors snapshot centering logic) */
    double page_x;
    if (self->max_width <= widget_width)
      page_x = (widget_width - pw) / 2.0;
    else
      page_x = (self->max_width - pw) / 2.0 - scroll_x;

    double rel_x = widget_x - page_x;
    double rel_y = abs_y - py;

    /* Check bounds */
    if (rel_x < 0 || rel_x > pw || rel_y < 0 || rel_y > ph)
      return FALSE;

    /* Convert from zoomed pixels to document points */
    if (out_page)  *out_page  = i;
    if (out_doc_x) *out_doc_x = rel_x / self->zoom;
    if (out_doc_y) *out_doc_y = rel_y / self->zoom;
    return TRUE;
  }

  return FALSE;
}

/* ── Text selection gestures ─────────────────────────────────────── */

static void
on_drag_begin (GtkGestureDrag *gesture, double start_x, double start_y,
               gpointer user_data)
{
  (void) gesture;
  FwView *self = FW_VIEW (user_data);

  int page;
  double doc_x, doc_y;

  if (!fw_view_widget_to_doc (self, start_x, start_y, &page, &doc_x, &doc_y))
    return;

  self->sel_page = page;
  self->sel_x0 = doc_x;
  self->sel_y0 = doc_y;
  self->sel_x1 = doc_x;
  self->sel_y1 = doc_y;
  self->selecting = TRUE;
  g_clear_pointer (&self->selected_text, g_free);
  FW_TRACE_VIEW ("drag begin: page=%d doc=(%.1f,%.1f)", page, doc_x, doc_y);
}

static void
on_drag_update (GtkGestureDrag *gesture, double offset_x, double offset_y,
                gpointer user_data)
{
  (void) gesture;
  FwView *self = FW_VIEW (user_data);

  if (!self->selecting)
    return;

  double start_x, start_y;
  gtk_gesture_drag_get_start_point (gesture, &start_x, &start_y);

  double cur_x = start_x + offset_x;
  double cur_y = start_y + offset_y;

  int page;
  double doc_x, doc_y;
  if (fw_view_widget_to_doc (self, cur_x, cur_y, &page, &doc_x, &doc_y)) {
    /* Only update if still on the same page (single-page selection) */
    if (page == self->sel_page) {
      self->sel_x1 = doc_x;
      self->sel_y1 = doc_y;
    }
  }

  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag *gesture, double offset_x, double offset_y,
             gpointer user_data)
{
  (void) gesture; (void) offset_x; (void) offset_y;
  FwView *self = FW_VIEW (user_data);

  if (!self->selecting || !self->document)
    return;

  self->selecting = FALSE;

  /* Normalize selection rect (ensure x0<x1, y0<y1) */
  double x0 = self->sel_x0 < self->sel_x1 ? self->sel_x0 : self->sel_x1;
  double y0 = self->sel_y0 < self->sel_y1 ? self->sel_y0 : self->sel_y1;
  double x1 = self->sel_x0 > self->sel_x1 ? self->sel_x0 : self->sel_x1;
  double y1 = self->sel_y0 > self->sel_y1 ? self->sel_y0 : self->sel_y1;

  g_clear_pointer (&self->selected_text, g_free);
  self->selected_text = fw_document_get_text (
    self->document, self->sel_page, x0, y0, x1, y1);

  FW_TRACE_VIEW ("drag end: page=%d rect=(%.1f,%.1f)-(%.1f,%.1f) text=%s",
                  self->sel_page, x0, y0, x1, y1,
                  self->selected_text ? "yes" : "no");
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ── Link cache ──────────────────────────────────────────────────── */

static void
fw_view_free_link_cache (FwView *self)
{
  if (!self->link_cache)
    return;
  for (int i = 0; i < self->link_cache_count; i++) {
    if (self->link_cache[i])
      g_array_unref (self->link_cache[i]);
  }
  g_free (self->link_cache);
  self->link_cache = NULL;
  self->link_cache_count = 0;
}

static GArray *
fw_view_get_links_for_page (FwView *self, int page)
{
  if (!self->document || page < 0 || page >= self->page_count)
    return NULL;

  /* Allocate the cache array on first access */
  if (!self->link_cache) {
    self->link_cache = g_new0 (GArray *, self->page_count);
    self->link_cache_count = self->page_count;
  }

  if (!self->link_cache[page])
    self->link_cache[page] = fw_document_get_links (self->document, page);

  return self->link_cache[page];
}

/* Check if a point (in document coordinates) hits a link on the given page.
 * Returns the link pointer or NULL. */
static FwLink *
fw_view_hit_test_link (FwView *self, int page, double doc_x, double doc_y)
{
  GArray *links = fw_view_get_links_for_page (self, page);
  if (!links)
    return NULL;

  for (guint i = 0; i < links->len; i++) {
    FwLink *link = g_array_index (links, FwLink *, i);
    if (doc_x >= link->x0 && doc_x <= link->x1 &&
        doc_y >= link->y0 && doc_y <= link->y1)
      return link;
  }
  return NULL;
}

/* ── Dynamic cursors ─────────────────────────────────────────────── */

static void
on_motion (GtkEventControllerMotion *controller, double x, double y,
           gpointer user_data)
{
  (void) controller;
  FwView *self = FW_VIEW (user_data);

  int page;
  double doc_x, doc_y;

  if (fw_view_widget_to_doc (self, x, y, &page, &doc_x, &doc_y)) {
    FwLink *link = fw_view_hit_test_link (self, page, doc_x, doc_y);
    if (link)
      gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "pointer");
    else
      gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "text");
  } else {
    gtk_widget_set_cursor (GTK_WIDGET (self), NULL);
  }
}

/* ── Link click handling ─────────────────────────────────────────── */

static void
on_click_pressed (GtkGestureClick *gesture, int n_press, double x, double y,
                  gpointer user_data)
{
  (void) n_press;
  FwView *self = FW_VIEW (user_data);

  int page;
  double doc_x, doc_y;

  if (!fw_view_widget_to_doc (self, x, y, &page, &doc_x, &doc_y))
    return;

  FwLink *link = fw_view_hit_test_link (self, page, doc_x, doc_y);
  if (!link)
    return;

  /* Claim the gesture so the drag gesture doesn't start text selection */
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  if (link->type == FW_LINK_INTERNAL) {
    FW_TRACE_VIEW ("link click: internal → page %d", link->dest_page);
    fw_view_go_to_page (self, link->dest_page);
  } else if (link->type == FW_LINK_EXTERNAL && link->uri) {
    FW_TRACE_VIEW ("link click: external → %s", link->uri);
    GtkUriLauncher *launcher = gtk_uri_launcher_new (link->uri);
    GtkWidget *toplevel = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (self)));
    gtk_uri_launcher_launch (launcher,
                             GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : NULL,
                             NULL, NULL, NULL);
    g_object_unref (launcher);
  }
}

/* ── Public API ───────────────────────────────────────────────────── */

FwView *
fw_view_new (void)
{
  return g_object_new (FW_TYPE_VIEW, NULL);
}

void
fw_view_set_document (FwView *self, FwDocument *document, FwCache *cache)
{
  g_return_if_fail (FW_IS_VIEW (self));

  g_set_object (&self->document, document);
  g_set_object (&self->cache, cache);
  self->page_count = document ? fw_document_get_page_count (document) : 0;

  /* Invalidate per-document caches */
  fw_view_free_link_cache (self);
  self->sel_page = -1;
  g_clear_pointer (&self->selected_text, g_free);

  FW_TRACE_VIEW ("set_document: pages=%d", self->page_count);
  recompute_layout (self);
  update_cache_priority (self);
}

void
fw_view_set_zoom (FwView *self, double zoom)
{
  g_return_if_fail (FW_IS_VIEW (self));
  FW_TRACE_VIEW ("set_zoom: %.2f → %.2f", self->zoom, zoom);

  /* Preserve scroll position across zoom changes */
  int anchor_page = -1;
  double anchor_frac = 0;

  if (self->vadjustment && self->page_y_offsets && self->page_count > 0) {
    double scroll_y = gtk_adjustment_get_value (self->vadjustment);
    anchor_page = fw_view_get_current_page (self);
    if (anchor_page >= 0 && anchor_page < self->page_count &&
        self->page_heights[anchor_page] > 0) {
      anchor_frac = (scroll_y - self->page_y_offsets[anchor_page])
                  / self->page_heights[anchor_page];
      if (anchor_frac < 0) anchor_frac = 0;
      if (anchor_frac > 1) anchor_frac = 1;
    }
  }

  self->zoom = zoom;
  recompute_layout (self);

  /* Restore: jump to the same page + fraction in the new layout */
  if (anchor_page >= 0 && self->vadjustment && self->page_y_offsets) {
    double new_val = self->page_y_offsets[anchor_page]
                   + anchor_frac * self->page_heights[anchor_page];
    FW_TRACE_VIEW ("scroll restore: page=%d frac=%.3f val=%.1f",
                    anchor_page, anchor_frac, new_val);
    gtk_adjustment_set_value (self->vadjustment, new_val);
  }
}

double
fw_view_fit_width_zoom (FwView *self, int viewport_w)
{
  g_return_val_if_fail (FW_IS_VIEW (self), 1.0);

  if (!self->document || self->page_count == 0)
    return 1.0;

  if (viewport_w <= 0)
    viewport_w = 900;

  /* Find the widest page (in points, accounting for rotation) */
  double max_page_w = 0;
  for (int i = 0; i < self->page_count; i++) {
    double w, h;
    fw_document_get_page_size (self->document, i, &w, &h);
    if (self->rotation == 90 || self->rotation == 270) {
      double tmp = w; w = h; h = tmp;
    }
    if (w > max_page_w) max_page_w = w;
  }

  if (max_page_w <= 0)
    return 1.0;

  return (double) viewport_w / max_page_w;
}

double
fw_view_fit_page_zoom (FwView *self, int viewport_w, int viewport_h)
{
  g_return_val_if_fail (FW_IS_VIEW (self), 1.0);

  if (!self->document || self->page_count == 0)
    return 1.0;

  if (viewport_w <= 0) viewport_w = 900;
  if (viewport_h <= 0) viewport_h = 700;

  /* Find the widest and tallest page (in points, accounting for rotation) */
  double max_page_w = 0;
  double max_page_h = 0;
  for (int i = 0; i < self->page_count; i++) {
    double w, h;
    fw_document_get_page_size (self->document, i, &w, &h);
    if (self->rotation == 90 || self->rotation == 270) {
      double tmp = w; w = h; h = tmp;
    }
    if (w > max_page_w) max_page_w = w;
    if (h > max_page_h) max_page_h = h;
  }

  if (max_page_w <= 0 || max_page_h <= 0)
    return 1.0;

  double zoom_w = (double) viewport_w / max_page_w;
  double zoom_h = (double) viewport_h / max_page_h;
  return zoom_w < zoom_h ? zoom_w : zoom_h;
}

void
fw_view_go_to_page (FwView *self, int page)
{
  g_return_if_fail (FW_IS_VIEW (self));

  if (!self->document || page < 0 || page >= self->page_count)
    return;
  if (!self->vadjustment || !self->page_y_offsets)
    return;

  gtk_adjustment_set_value (self->vadjustment, self->page_y_offsets[page]);
}

int
fw_view_get_current_page (FwView *self)
{
  g_return_val_if_fail (FW_IS_VIEW (self), 0);

  if (!self->vadjustment || !self->page_y_offsets || self->page_count == 0)
    return 0;

  double scroll_y = gtk_adjustment_get_value (self->vadjustment);

  /* Find the page whose top is closest to (but not past) the viewport top */
  for (int i = self->page_count - 1; i >= 0; i--) {
    if (self->page_y_offsets[i] <= scroll_y + 1.0)
      return i;
  }
  return 0;
}

void
fw_view_set_invert (FwView *self, gboolean invert)
{
  g_return_if_fail (FW_IS_VIEW (self));
  if (self->invert_colors == invert)
    return;
  self->invert_colors = invert;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
fw_view_set_rotation (FwView *self, int rotation)
{
  g_return_if_fail (FW_IS_VIEW (self));

  /* Clamp to valid values */
  rotation = ((rotation % 360) + 360) % 360;
  if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
    rotation = 0;

  if (self->rotation == rotation)
    return;

  FW_TRACE_VIEW ("set_rotation: %d → %d", self->rotation, rotation);
  self->rotation = rotation;
  recompute_layout (self);
}

const char *
fw_view_get_selected_text (FwView *self)
{
  g_return_val_if_fail (FW_IS_VIEW (self), NULL);
  return self->selected_text;
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_view_dispose (GObject *object)
{
  FwView *self = FW_VIEW (object);

  fw_view_free_link_cache (self);
  g_clear_object (&self->document);
  g_clear_object (&self->cache);

  if (self->hadjustment) {
    g_signal_handlers_disconnect_by_func (self->hadjustment,
                                          adjustment_value_changed, self);
    g_clear_object (&self->hadjustment);
  }
  if (self->vadjustment) {
    g_signal_handlers_disconnect_by_func (self->vadjustment,
                                          adjustment_value_changed, self);
    g_clear_object (&self->vadjustment);
  }

  G_OBJECT_CLASS (fw_view_parent_class)->dispose (object);
}

static void
fw_view_finalize (GObject *object)
{
  FwView *self = FW_VIEW (object);
  g_free (self->page_widths);
  g_free (self->page_heights);
  g_free (self->page_y_offsets);
  g_free (self->selected_text);
  G_OBJECT_CLASS (fw_view_parent_class)->finalize (object);
}

static void
fw_view_class_init (FwViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose      = fw_view_dispose;
  object_class->finalize     = fw_view_finalize;
  object_class->set_property = fw_view_set_property;
  object_class->get_property = fw_view_get_property;

  widget_class->measure       = fw_view_measure;
  widget_class->size_allocate = fw_view_size_allocate;
  widget_class->snapshot      = fw_view_snapshot;

  /* GtkScrollable properties — must override, not install new */
  g_object_class_override_property (object_class, PROP_HADJUSTMENT,  "hadjustment");
  g_object_class_override_property (object_class, PROP_VADJUSTMENT,  "vadjustment");
  g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
}

static gboolean
view_tick_cb (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  (void) user_data;
  FwView *self = FW_VIEW (widget);
  gint64 time = gdk_frame_clock_get_frame_time (clock);
  
  if (self->vadjustment && self->cache) {
    double scroll_y = gtk_adjustment_get_value (self->vadjustment);

    if (self->last_frame_time > 0) {
      double dy = scroll_y - self->last_scroll_y;
      double dt = (time - self->last_frame_time) / 1000.0; /* microseconds to ms */
      
      if (dt > 0) {
        double raw_velocity = fabs (dy / dt);
        /* Exponential moving average to prevent single-frame spikes
         * from triggering scrubbing abort (e.g., trackpad jitter) */
        self->smooth_velocity = 0.7 * self->smooth_velocity
                              + 0.3 * raw_velocity;
        if (fw_cache_set_velocity (self->cache, self->smooth_velocity)) {
          update_cache_priority (self);
        }
      }
    }

    self->last_scroll_y = scroll_y;
  }

  self->last_frame_time = time;
  
  return G_SOURCE_CONTINUE;
}

static void
fw_view_init (FwView *self)
{
  self->zoom = 1.0;
  self->sel_page = -1;

  gtk_widget_add_tick_callback (GTK_WIDGET (self), view_tick_cb, self, NULL);

  /* Link click handler — must be added before drag so it can claim
   * the gesture sequence when clicking a link. */
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  g_signal_connect (click, "pressed", G_CALLBACK (on_click_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

  /* Text selection via click-drag */
  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
  g_signal_connect (drag, "drag-begin",  G_CALLBACK (on_drag_begin),  self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end",    G_CALLBACK (on_drag_end),    self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

  /* Dynamic cursor: hand on links, text cursor on content */
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);
}
