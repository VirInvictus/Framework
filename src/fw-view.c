/* fw-view.c — Custom page rendering widget
 *
 * Implements GtkScrollable for proper integration with GtkScrolledWindow.
 * Lays out rendered pages vertically in continuous scroll mode.
 * Only paints pages visible in the current viewport.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-view.h"

#define PAGE_GAP 8

struct _FwView {
  GtkWidget    parent_instance;

  FwDocument  *document;
  FwCache     *cache;

  double       zoom;
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
  gtk_widget_queue_draw (GTK_WIDGET (self));
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

    cairo_surface_t *surface = fw_cache_get_page (self->cache, i);

    if (surface) {
      int sw = cairo_image_surface_get_width (surface);
      int sh = cairo_image_surface_get_height (surface);

      graphene_rect_t rect = GRAPHENE_RECT_INIT ((float) x, (float) y,
                                                  (float) pw, (float) ph);

      cairo_surface_flush (surface);
      int stride = cairo_image_surface_get_stride (surface);
      unsigned char *data = cairo_image_surface_get_data (surface);
      GBytes *bytes = g_bytes_new (data, (gsize) stride * (gsize) sh);

      GdkTexture *texture = GDK_TEXTURE (
        gdk_memory_texture_new (sw, sh,
                                GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                bytes, (gsize) stride));
      g_bytes_unref (bytes);

      gtk_snapshot_append_texture (snapshot, texture, &rect);
      g_object_unref (texture);
      cairo_surface_destroy (surface);
    } else {
      graphene_rect_t rect = GRAPHENE_RECT_INIT ((float) x, (float) y,
                                                  (float) pw, (float) ph);
      GdkRGBA gray = { 0.92f, 0.92f, 0.92f, 1.0f };
      gtk_snapshot_append_color (snapshot, &gray, &rect);
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

  self->document   = document;
  self->cache      = cache;
  self->page_count = document ? fw_document_get_page_count (document) : 0;

  recompute_layout (self);
  update_cache_priority (self);
}

void
fw_view_set_zoom (FwView *self, double zoom)
{
  g_return_if_fail (FW_IS_VIEW (self));
  self->zoom = zoom;
  recompute_layout (self);
}

double
fw_view_fit_width_zoom (FwView *self, int viewport_w)
{
  g_return_val_if_fail (FW_IS_VIEW (self), 1.0);

  if (!self->document || self->page_count == 0)
    return 1.0;

  if (viewport_w <= 0)
    viewport_w = 900;

  /* Find the widest page (in points) and compute zoom to fill viewport */
  double max_page_w = 0;
  for (int i = 0; i < self->page_count; i++) {
    double w, h;
    fw_document_get_page_size (self->document, i, &w, &h);
    if (w > max_page_w) max_page_w = w;
  }

  if (max_page_w <= 0)
    return 1.0;

  return (double) viewport_w / max_page_w;
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

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_view_dispose (GObject *object)
{
  FwView *self = FW_VIEW (object);

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
  FwView *self = FW_VIEW (widget);
  gint64 time = gdk_frame_clock_get_frame_time (clock);
  
  if (self->vadjustment && self->cache) {
    double scroll_y = gtk_adjustment_get_value (self->vadjustment);

    if (self->last_frame_time > 0) {
      double dy = scroll_y - self->last_scroll_y;
      double dt = (time - self->last_frame_time) / 1000.0; /* microseconds to ms */
      
      if (dt > 0) {
        double velocity = fabs (dy / dt);
        if (fw_cache_set_velocity (self->cache, velocity)) {
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
  gtk_widget_add_tick_callback (GTK_WIDGET (self), view_tick_cb, self, NULL);
}
