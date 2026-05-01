/* fw-view.c — Custom page rendering widget
 *
 * Implements GtkScrollable for proper integration with GtkScrolledWindow.
 * Lays out rendered pages vertically in continuous scroll mode.
 * Only paints pages visible in the current viewport.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-config.h"
#include "fw-view.h"
#include "fw-debug.h"

#include <gio/gio.h>

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

  /* Pair-partner array, populated by recompute_layout when
   * facing-pages is on. pair_partner[i] == i means standalone (cover,
   * spread, or orphaned-by-spread); else pair_partner[i] == j means i
   * is paired with j. NULL when facing-pages is off. Built from
   * aspect-ratio-based spread detection so wide centerfold pages
   * (single landscape image == one logical 2-page spread) stand alone
   * instead of getting paired with the adjacent portrait page. */
  int         *pair_partner;

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

  /* Text selection
   * sel_x0/y0 + sel_x1/y1 are the user's drag start/end (or snap-result
   * endpoints for double/triple-click). sel_quads is the per-line
   * highlight rectangles MuPDF returns from fz_highlight_selection —
   * one quad per line of selected text, following reading order, so the
   * highlight matches the actual selected text rather than drawing a
   * single bounding box that may include unselected words. */
  int          sel_page;       /* -1 = no selection */
  double       sel_x0, sel_y0; /* start in document points */
  double       sel_x1, sel_y1; /* end in document points */
  gboolean     selecting;
  char        *selected_text;
  GArray      *sel_quads;      /* GArray<FwRect>, owned, NULL when empty */

  /* Link cache — lazily populated per page */
  GArray     **link_cache;     /* array of GArray* indexed by page, or NULL */
  int          link_cache_count;

  /* Search highlighting */
  FwSearch    *search;         /* owned ref, or NULL */
  gulong       search_hits_handler;
  gulong       search_current_handler;

  /* Settings drive the reading-ruler and loupe live-toggles.
   * Kinetic-scrolling is handled by FwWindow on the parent scrolled
   * window — view doesn't need to know about it. */
  GSettings   *settings;
  gulong       ruler_changed_handler;

  /* Reading ruler — when active, paint a dark dimming overlay over the
   * whole widget with a clear horizontal band tracking the mouse Y.
   * Helps keep the eye on the active line in dense technical reading. */
  gboolean     reading_ruler;
  double       ruler_y;          /* widget-coordinate Y of the cursor */
  gboolean     ruler_y_valid;    /* FALSE before first motion event */

  /* Magnifying loupe — circular zoomed view at the cursor. Tracks both
   * X and Y. Same source-of-truth flag as the ruler (mouse position) but
   * the loupe needs both axes whereas the ruler only uses Y. */
  gboolean     loupe;
  double       cursor_x;
  double       cursor_y;
  gboolean     cursor_valid;
  gulong       loupe_changed_handler;

  /* Margin crop — when active, the layout shrinks pages to their
   * inked-content bbox (computed from the current visible page on
   * activation) and the snapshot positions the full texture so the
   * content area fills the cropped rect. Fractions are 0..1 of the
   * full page in document coords; identical fractions are applied
   * across all pages (assumes uniform margins, which holds for
   * 99% of technical PDFs). */
  gboolean     crop_margins;
  gboolean     crop_fractions_valid;
  double       crop_l_frac;     /* fraction of width trimmed from left */
  double       crop_r_frac;     /* fraction of width trimmed from right */
  double       crop_t_frac;     /* fraction of height trimmed from top */
  double       crop_b_frac;     /* fraction of height trimmed from bottom */
  gulong       crop_changed_handler;

  /* Comic reading modes (Phase 13). All three are layout-affecting
   * GSettings booleans toggleable from the menu and via F-keys; they
   * compose so e.g. manga + facing-pages flips pair order while keeping
   * the side-by-side layout. */
  gboolean     manga_mode;      /* RTL nav: Left=next, Right=prev */
  gboolean     webtoon_mode;    /* PAGE_GAP=0 — seamless vertical strip */
  gboolean     facing_pages;    /* two pages per row, page 0 standalone */
  gulong       manga_changed_handler;
  gulong       webtoon_changed_handler;
  gulong       facing_changed_handler;
};

static void fw_view_scrollable_init (GtkScrollableInterface *iface);
static gboolean fw_view_widget_to_doc (FwView *self,
                                       double widget_x, double widget_y,
                                       int *out_page,
                                       double *out_doc_x, double *out_doc_y);

G_DEFINE_FINAL_TYPE_WITH_CODE (FwView, fw_view, GTK_TYPE_WIDGET,
  G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, fw_view_scrollable_init))

enum {
  SIGNAL_PAGE_JUMPED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

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
  double scroll_y   = gtk_adjustment_get_value (self->vadjustment);

  /* During state restore, the scroll position may be set before the view
   * has a real allocation. Fall back to the page at the scroll position
   * so the cache still gets a priority hint for the saved page — without
   * this, the saved page stays at thumbnail resolution until the user
   * scrolls (the v0.13 startup-blur regression). */
  if (widget_height <= 0) {
    int page = 0;
    for (int i = 0; i < self->page_count; i++) {
      if (self->page_y_offsets[i] <= scroll_y + 1.0)
        page = i;
      else
        break;
    }
    int single[1] = { page };
    fw_cache_set_priority (self->cache, single, 1);
    return;
  }

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

/* Pair-aware layout helpers — kept tight so the rest of the view code
 * doesn't have to reason about facing-pages mode. The base convention
 * is "page 0 stands alone (cover); pages 1+2, 3+4, ... pair up", but
 * pages with a landscape-or-square aspect ratio are treated as
 * pre-rendered spreads and stand alone, with the adjacent page
 * orphaning to resume the alternation. The actual pairing decisions
 * are baked into pair_partner[] during recompute_layout. */
static gboolean
view_page_is_spread (FwView *self, int i)
{
  if (i < 0 || i >= self->page_count
      || !self->page_widths || !self->page_heights)
    return FALSE;
  double w = self->page_widths[i];
  double h = self->page_heights[i];
  if (h <= 0)
    return FALSE;
  /* Normal manga / comic / textbook pages are portrait (w/h ~ 0.65-0.80).
   * Centerfold or 2-page-spread images are square or wider. The
   * threshold of 1.0 cleanly catches landscape spreads while leaving
   * every reasonable portrait page as pair-eligible. Books that are
   * landscape end-to-end (artbooks, photo collections) will see every
   * page standalone — appropriate, since pairing pre-spread pages
   * would just shrink them in half. */
  return (w / h) > 1.0;
}

static gboolean
view_page_is_paired (FwView *self, int i)
{
  if (!self->facing_pages || !self->pair_partner)
    return FALSE;
  if (i < 0 || i >= self->page_count)
    return FALSE;
  return self->pair_partner[i] != i;
}

static int
view_pair_first (FwView *self, int i)
{
  if (!view_page_is_paired (self, i))
    return i;
  int j = self->pair_partner[i];
  return (i < j) ? i : j;
}

static void
recompute_layout (FwView *self)
{
  if (!self->document || self->page_count == 0)
    return;

  g_free (self->page_widths);
  g_free (self->page_heights);
  g_free (self->page_y_offsets);
  g_free (self->pair_partner);

  self->page_widths    = g_new (double, self->page_count);
  self->page_heights   = g_new (double, self->page_count);
  self->page_y_offsets = g_new (double, self->page_count);
  self->pair_partner   = g_new (int, self->page_count);
  self->total_height = 0;
  self->max_width    = 0;

  /* Per-page intrinsic geometry first — y_offsets are computed in a
   * second pass so facing-pages can pair adjacent rows. */
  for (int i = 0; i < self->page_count; i++) {
    double w, h;
    fw_document_get_page_size (self->document, i, &w, &h);

    /* Swap dimensions for 90/270 degree rotation */
    if (self->rotation == 90 || self->rotation == 270) {
      double tmp = w;
      w = h;
      h = tmp;
    }

    /* Margin crop applies the same fractional trim to every page —
     * page_widths / page_heights become the *visible* size. The
     * snapshot path uses these cropped dimensions for layout, then
     * positions the full texture so the content area fills them. */
    double crop_w_factor = 1.0;
    double crop_h_factor = 1.0;
    if (self->crop_margins && self->crop_fractions_valid) {
      crop_w_factor = 1.0 - self->crop_l_frac - self->crop_r_frac;
      crop_h_factor = 1.0 - self->crop_t_frac - self->crop_b_frac;
      /* Sanity: never collapse below 10% of original (degenerate
       * stext bbox shouldn't murder the layout). */
      if (crop_w_factor < 0.1) crop_w_factor = 1.0;
      if (crop_h_factor < 0.1) crop_h_factor = 1.0;
    }

    self->page_widths[i]  = w * self->zoom * crop_w_factor;
    self->page_heights[i] = h * self->zoom * crop_h_factor;
  }

  /* Build pair_partner first (depends only on page sizes, not y), so
   * the row loop below can consult it. Decisions:
   *   - facing-pages off → every page standalone.
   *   - i == 0 → cover, standalone.
   *   - i is a spread → standalone.
   *   - i+1 doesn't exist or is a spread → i orphaned, standalone.
   *   - otherwise → pair (i, i+1), advance by 2.
   * pair_partner[i] == i is the "standalone" sentinel. */
  for (int k = 0; k < self->page_count; k++)
    self->pair_partner[k] = k;

  if (self->facing_pages) {
    int k = 1;  /* page 0 is always the standalone cover */
    while (k < self->page_count) {
      if (view_page_is_spread (self, k)) {
        k += 1;
        continue;
      }
      if (k + 1 < self->page_count
          && !view_page_is_spread (self, k + 1)) {
        self->pair_partner[k]     = k + 1;
        self->pair_partner[k + 1] = k;
        k += 2;
      } else {
        k += 1;  /* page-before-spread or final orphan */
      }
    }
  }

  /* Webtoon mode stitches pages with no inter-page gap; facing-pages
   * keeps the gap between rows (and a fixed gutter between paired
   * pages, drawn in the snapshot path). */
  double row_gap = self->webtoon_mode ? 0.0 : PAGE_GAP;

  int i = 0;
  while (i < self->page_count) {
    if (self->pair_partner[i] == i + 1) {
      /* Pair (i, i+1) — both share the same y_offset; row height is
       * the taller of the two; pair width includes the gutter so
       * max_width tracks paired layouts correctly. */
      double pair_h = MAX (self->page_heights[i], self->page_heights[i + 1]);
      double pair_w = self->page_widths[i] + PAGE_GAP + self->page_widths[i + 1];

      self->page_y_offsets[i]     = self->total_height;
      self->page_y_offsets[i + 1] = self->total_height;
      self->total_height += pair_h;

      if (pair_w > self->max_width)
        self->max_width = pair_w;

      i += 2;
    } else {
      /* Standalone — cover, spread, or orphan-before-spread. */
      self->page_y_offsets[i] = self->total_height;
      self->total_height += self->page_heights[i];
      if (self->page_widths[i] > self->max_width)
        self->max_width = self->page_widths[i];
      i += 1;
    }
    if (i < self->page_count)
      self->total_height += row_gap;
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

/* Per-second snapshot stats. Only used when FW_DEBUG=1 — zero-overhead
 * when disabled. Counts are local to this translation unit; they sum
 * frame work for the active FwView only. */
static gint64 g_snap_window_start_us = 0;
static int    g_snap_count           = 0;
static gint64 g_snap_total_us        = 0;
static int    g_snap_loupe_count     = 0;

static void
fw_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  FwView *self = FW_VIEW (widget);
  self->redraw_pending = FALSE;

  if (!self->document || !self->cache || !self->page_y_offsets)
    return;

  gint64 frame_start = g_get_monotonic_time ();

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

    /* Position the page horizontally. Per-page sizing rather than
     * document-wide so a comic with one wide centerfold doesn't push
     * every normal page off-center. If THIS page fits in the viewport,
     * center it there; if it's wider than the viewport, fall back to
     * canvas-relative positioning (centered in `max_width`) so the
     * horizontal scrollbar still spans the doc's widest content.
     *
     * Facing-pages mode overrides the single-page centering: the row
     * is the pair (left | right) with a fixed gutter, centered as a
     * unit. Manga mode flips left/right within the pair. */
    double x;
    if (view_page_is_paired (self, i)) {
      int first  = view_pair_first (self, i);
      int second = first + 1;
      double pw_first  = self->page_widths[first];
      double pw_second = self->page_widths[second];
      double pair_w    = pw_first + PAGE_GAP + pw_second;

      double pair_x;
      if (pair_w <= widget_width)
        pair_x = (widget_width - pair_w) / 2.0;
      else
        pair_x = (self->max_width - pair_w) / 2.0 - scroll_x;

      /* Manga reading order: lower-numbered page on the right, higher
       * on the left. Otherwise: lower on the left. */
      double x_first, x_second;
      if (self->manga_mode) {
        x_first  = pair_x + pw_second + PAGE_GAP;
        x_second = pair_x;
      } else {
        x_first  = pair_x;
        x_second = pair_x + pw_first + PAGE_GAP;
      }
      x = (i == first) ? x_first : x_second;
    } else if (pw <= widget_width) {
      x = (widget_width - pw) / 2.0;
    } else {
      x = (self->max_width - pw) / 2.0 - scroll_x;
    }
    double y = py - scroll_y;

    graphene_rect_t rect = GRAPHENE_RECT_INIT ((float) x, (float) y,
                                                (float) pw, (float) ph);

    /* When margin-cropping is active, the texture (rendered at full
     * page size) needs to be drawn larger than `rect` and offset so
     * its content area aligns with `rect`. The full texture rect is
     * computed from pw/ph divided by the crop factors; the offset is
     * the margin fraction × full size. A clip rect prevents the
     * margins from leaking past the cropped page rect. */
    gboolean cropping = self->crop_margins && self->crop_fractions_valid;
    graphene_rect_t tex_rect = rect;
    if (cropping) {
      double w_factor = 1.0 - self->crop_l_frac - self->crop_r_frac;
      double h_factor = 1.0 - self->crop_t_frac - self->crop_b_frac;
      if (w_factor > 0.1 && h_factor > 0.1) {
        double full_pw = pw / w_factor;
        double full_ph = ph / h_factor;
        double off_x   = full_pw * self->crop_l_frac;
        double off_y   = full_ph * self->crop_t_frac;
        tex_rect = GRAPHENE_RECT_INIT (
          (float) (x - off_x), (float) (y - off_y),
          (float) full_pw,    (float) full_ph);
      }
    }

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
        /* Hue-preserving lightness inversion (Phase 11 Tier 2).
         *
         * The previous implementation was a bitwise NOT (R'=1-R etc.),
         * which inverts every channel independently — that turns red
         * diagrams into cyan and code-syntax red strings into cyan
         * strings, destroying the document's chromatic information.
         *
         * The replacement computes BT.601 luma Y = 0.299R+0.587G+0.114B
         * and remaps it as new_Y = 1-Y, then adjusts each channel by
         * the same delta so the chromatic component (R-Y, G-Y, B-Y)
         * is preserved. Red stays red, just on a dark background;
         * blue plots stay blue; black text becomes white.
         *
         * As a 4×4 affine transform with offset (1,1,1,0):
         *   R' = R + (1 - 2Y) = 0.402R − 1.174G − 0.228B + 1
         *   G' = G + (1 - 2Y) = −0.598R − 0.174G − 0.228B + 1
         *   B' = B + (1 - 2Y) = −0.598R − 1.174G + 0.772B + 1
         *
         * graphene_matrix_init_from_float takes column-major order, so
         * each line below is one input-channel's contribution to all
         * output channels. GSK clamps out-of-gamut output to [0,1]. */
        graphene_matrix_t color_matrix;
        graphene_matrix_init_from_float (&color_matrix,
          (const float[16]) {
             0.402f, -0.598f, -0.598f, 0,  /* R input → R',G',B',A' */
            -1.174f, -0.174f, -1.174f, 0,  /* G input */
            -0.228f, -0.228f,  0.772f, 0,  /* B input */
             0,       0,       0,       1, /* A input */
          });
        graphene_vec4_t color_offset;
        graphene_vec4_init (&color_offset, 1.0f, 1.0f, 1.0f, 0.0f);
        gtk_snapshot_push_color_matrix (snapshot, &color_matrix, &color_offset);
      }

      if (cropping)
        gtk_snapshot_push_clip (snapshot, &rect);

      gtk_snapshot_append_texture (snapshot, texture, &tex_rect);

      if (cropping)
        gtk_snapshot_pop (snapshot); /* pop clip */

      if (self->invert_colors)
        gtk_snapshot_pop (snapshot);
      /* texture is borrowed from the cache — do not unref */
    } else {
      GdkRGBA gray = { 0.92f, 0.92f, 0.92f, 1.0f };
      gtk_snapshot_append_color (snapshot, &gray, &rect);
    }

    /* Paint search match overlays on this page */
    if (self->search) {
      int active_in_page = -1;
      GArray *page_hits = fw_search_hits_for_page (self->search, i,
                                                    &active_in_page);
      if (page_hits) {
        for (guint h = 0; h < page_hits->len; h++) {
          FwSearchHit *hit = &g_array_index (page_hits, FwSearchHit, h);
          float hx = (float) (x + hit->x0 * self->zoom);
          float hy = (float) (y + hit->y0 * self->zoom);
          float hw = (float) ((hit->x1 - hit->x0) * self->zoom);
          float hh = (float) ((hit->y1 - hit->y0) * self->zoom);
          if (hw <= 0 || hh <= 0)
            continue;
          graphene_rect_t hr = GRAPHENE_RECT_INIT (hx, hy, hw, hh);
          GdkRGBA color = (int) h == active_in_page
            ? (GdkRGBA) { 1.0f, 0.55f, 0.10f, 0.55f }    /* active: orange */
            : (GdkRGBA) { 1.0f, 0.92f, 0.20f, 0.40f };   /* match: yellow */
          gtk_snapshot_append_color (snapshot, &color, &hr);
        }
        g_array_unref (page_hits);
      }
    }

    /* Paint text selection overlay on the selected page. Prefer the
     * per-line quads from fz_highlight_selection (they follow reading
     * order and match the actual selected text exactly). Fall back to
     * the bounding box only when quads aren't available (DjVu, CBR
     * — backends that don't implement get_selection_quads). */
    if (self->sel_page == i &&
        (self->selecting || self->selected_text)) {
      GdkRGBA sel_color = { 0.2f, 0.4f, 0.8f, 0.3f };

      if (self->sel_quads && self->sel_quads->len > 0) {
        for (guint q = 0; q < self->sel_quads->len; q++) {
          FwRect r = g_array_index (self->sel_quads, FwRect, q);
          float qx = (float) (x + r.x0 * self->zoom);
          float qy = (float) (y + r.y0 * self->zoom);
          float qw = (float) ((r.x1 - r.x0) * self->zoom);
          float qh = (float) ((r.y1 - r.y0) * self->zoom);
          if (qw > 0 && qh > 0) {
            graphene_rect_t qr = GRAPHENE_RECT_INIT (qx, qy, qw, qh);
            gtk_snapshot_append_color (snapshot, &sel_color, &qr);
          }
        }
      } else {
        double sx0 = self->sel_x0 < self->sel_x1 ? self->sel_x0 : self->sel_x1;
        double sy0 = self->sel_y0 < self->sel_y1 ? self->sel_y0 : self->sel_y1;
        double sx1 = self->sel_x0 > self->sel_x1 ? self->sel_x0 : self->sel_x1;
        double sy1 = self->sel_y0 > self->sel_y1 ? self->sel_y0 : self->sel_y1;
        float sel_wx = (float) (x + sx0 * self->zoom);
        float sel_wy = (float) (y + sy0 * self->zoom);
        float sel_ww = (float) ((sx1 - sx0) * self->zoom);
        float sel_wh = (float) ((sy1 - sy0) * self->zoom);
        if (sel_ww > 0 && sel_wh > 0) {
          graphene_rect_t sel_rect = GRAPHENE_RECT_INIT (
            sel_wx, sel_wy, sel_ww, sel_wh);
          gtk_snapshot_append_color (snapshot, &sel_color, &sel_rect);
        }
      }
    }
  }

  /* Magnifying loupe: a circular zoomed view at the cursor. Re-paints
   * the page texture under the cursor inside a rounded clip, with a
   * scale-around-cursor transform so what's at widget (cursor_x,
   * cursor_y) before scaling stays at the same widget point after.
   * Painted before the reading ruler so the ruler can dim the loupe
   * area too if both are active (intentional — the loupe stays bright
   * inside its clip; the ruler only dims around it). */
  if (self->loupe && self->cursor_valid) {
    int page_under_cursor;
    double doc_x, doc_y;
    if (fw_view_widget_to_doc (self, self->cursor_x, self->cursor_y,
                                &page_under_cursor, &doc_x, &doc_y)) {
      GdkTexture *tex = fw_cache_get_texture (self->cache, page_under_cursor);
      if (tex) {
        g_snap_loupe_count++;
        const float radius = 80.0f;
        const float zoom_factor = 2.5f;

        /* Recompute the widget-coords rect for this page (mirrors the
         * snapshot loop math). */
        double pw = self->page_widths[page_under_cursor];
        double ph = self->page_heights[page_under_cursor];
        double py = self->page_y_offsets[page_under_cursor];
        double page_widget_x;
        if (pw <= widget_width)
          page_widget_x = (widget_width - pw) / 2.0;
        else
          page_widget_x = (self->max_width - pw) / 2.0 - scroll_x;
        double page_widget_y = py - scroll_y;

        graphene_rect_t loupe_rect = GRAPHENE_RECT_INIT (
          (float) self->cursor_x - radius,
          (float) self->cursor_y - radius,
          radius * 2, radius * 2);

        /* Rounded clip — corner radius == half the side gives a circle. */
        GskRoundedRect rounded;
        gsk_rounded_rect_init_from_rect (&rounded, &loupe_rect, radius);
        gtk_snapshot_push_rounded_clip (snapshot, &rounded);

        /* Zoom-around-cursor: T(c) ∘ S ∘ T(-c). GSK transforms compose
         * outside-in: the last push is applied first to drawn content. */
        gtk_snapshot_save (snapshot);
        graphene_point_t c = { (float) self->cursor_x, (float) self->cursor_y };
        graphene_point_t neg_c = { -(float) self->cursor_x, -(float) self->cursor_y };
        gtk_snapshot_translate (snapshot, &c);
        gtk_snapshot_scale (snapshot, zoom_factor, zoom_factor);
        gtk_snapshot_translate (snapshot, &neg_c);

        graphene_rect_t page_rect = GRAPHENE_RECT_INIT (
          (float) page_widget_x, (float) page_widget_y,
          (float) pw, (float) ph);
        gtk_snapshot_append_texture (snapshot, tex, &page_rect);

        gtk_snapshot_restore (snapshot);
        gtk_snapshot_pop (snapshot); /* pop rounded clip */

        /* Thin border so the loupe is visible against the page. */
        GskRoundedRect outline = rounded;
        const GdkRGBA border_color = { 0.0f, 0.0f, 0.0f, 0.6f };
        const GdkRGBA border_colors[4] = { border_color, border_color,
                                            border_color, border_color };
        const float border_widths[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
        gtk_snapshot_append_border (snapshot, &outline,
                                     border_widths, border_colors);
      }
    }
  }

  /* Reading-ruler overlay: dim everything except a horizontal band that
   * tracks the cursor. Painted last so it sits above pages, selection,
   * and search highlights. The clear band is opt-out by simply not
   * painting over it; we paint two dark rects above and below. */
  if (self->reading_ruler && self->ruler_y_valid) {
    int widget_w = gtk_widget_get_width (widget);
    int widget_h = gtk_widget_get_height (widget);
    if (widget_w > 0 && widget_h > 0) {
      const float band_half = 28.0f;     /* ~one printed line at default zoom */
      float top    = (float) self->ruler_y - band_half;
      float bottom = (float) self->ruler_y + band_half;
      if (top < 0) top = 0;
      if (bottom > (float) widget_h) bottom = (float) widget_h;

      GdkRGBA dim = { 0.0f, 0.0f, 0.0f, 0.55f };

      if (top > 0) {
        graphene_rect_t r_top = GRAPHENE_RECT_INIT (0, 0, (float) widget_w, top);
        gtk_snapshot_append_color (snapshot, &dim, &r_top);
      }
      if (bottom < (float) widget_h) {
        graphene_rect_t r_bot = GRAPHENE_RECT_INIT (
          0, bottom, (float) widget_w, (float) widget_h - bottom);
        gtk_snapshot_append_color (snapshot, &dim, &r_bot);
      }
    }
  }

  /* Per-frame timing — accumulated and reported once per second when
   * FW_DEBUG=1. Lets us see whether the loupe is producing a frame
   * storm (high snap count) or expensive frames (high avg duration). */
  if (G_UNLIKELY (fw_debug_enabled ())) {
    gint64 frame_end = g_get_monotonic_time ();
    g_snap_count++;
    g_snap_total_us += (frame_end - frame_start);
    if (g_snap_window_start_us == 0) g_snap_window_start_us = frame_end;
    if (frame_end - g_snap_window_start_us >= 1000000) {
      double avg_ms = g_snap_count > 0
        ? (double) g_snap_total_us / 1000.0 / g_snap_count
        : 0.0;
      FW_TRACE_VIEW ("snap stats: %d frames/s avg=%.2fms loupe-paints=%d "
                     "ruler=%d loupe=%d",
                     g_snap_count, avg_ms, g_snap_loupe_count,
                     self->reading_ruler, self->loupe);
      g_snap_window_start_us = frame_end;
      g_snap_count = 0;
      g_snap_total_us = 0;
      g_snap_loupe_count = 0;
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

    /* Compute page X position (mirrors snapshot centering logic).
     * In facing-pages mode the row is the pair, so we resolve x via
     * the same pair-aware path the snapshot uses; otherwise we center
     * the standalone page. */
    double page_x;
    if (view_page_is_paired (self, i)) {
      int first  = view_pair_first (self, i);
      int second = first + 1;
      double pw_first  = self->page_widths[first];
      double pw_second = self->page_widths[second];
      double pair_w    = pw_first + PAGE_GAP + pw_second;
      double pair_x;
      if (pair_w <= widget_width)
        pair_x = (widget_width - pair_w) / 2.0;
      else
        pair_x = (self->max_width - pair_w) / 2.0 - scroll_x;

      if (self->manga_mode)
        page_x = (i == first) ? pair_x + pw_second + PAGE_GAP : pair_x;
      else
        page_x = (i == first) ? pair_x : pair_x + pw_first + PAGE_GAP;
    } else if (pw <= widget_width) {
      page_x = (widget_width - pw) / 2.0;
    } else {
      page_x = (self->max_width - pw) / 2.0 - scroll_x;
    }

    double rel_x = widget_x - page_x;
    double rel_y = abs_y - py;

    /* Check bounds */
    if (rel_x < 0 || rel_x > pw || rel_y < 0 || rel_y > ph)
      continue;  /* may be the other half of a pair */

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
  if (self->sel_quads) {
    g_array_unref (self->sel_quads);
    self->sel_quads = NULL;
  }
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

      /* Recompute per-line quads so the highlight follows reading
       * order in real time as the user drags. The cached stext makes
       * this fast — fz_highlight_selection is a stext walk, no parse. */
      if (self->document) {
        if (self->sel_quads) {
          g_array_unref (self->sel_quads);
          self->sel_quads = NULL;
        }
        self->sel_quads = fw_document_get_selection_quads (
          self->document, self->sel_page,
          self->sel_x0, self->sel_y0,
          self->sel_x1, self->sel_y1);
      }
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

  /* Final per-line quads for the released selection. */
  if (self->sel_quads) {
    g_array_unref (self->sel_quads);
    self->sel_quads = NULL;
  }
  self->sel_quads = fw_document_get_selection_quads (
    self->document, self->sel_page, x0, y0, x1, y1);

  FW_TRACE_VIEW ("drag end: page=%d rect=(%.1f,%.1f)-(%.1f,%.1f) text=%s quads=%u",
                  self->sel_page, x0, y0, x1, y1,
                  self->selected_text ? "yes" : "no",
                  self->sel_quads ? self->sel_quads->len : 0);
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

  /* Track cursor for ruler and loupe — both are widget-coordinate
   * overlays painted in the snapshot path. */
  self->cursor_x = x;
  self->cursor_y = y;
  self->cursor_valid = TRUE;

  if (self->reading_ruler) {
    self->ruler_y = y;
    self->ruler_y_valid = TRUE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
  if (self->loupe) {
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

/* ── Link click handling ─────────────────────────────────────────── */

static void
on_click_pressed (GtkGestureClick *gesture, int n_press, double x, double y,
                  gpointer user_data)
{
  FwView *self = FW_VIEW (user_data);

  int page;
  double doc_x, doc_y;

  if (!fw_view_widget_to_doc (self, x, y, &page, &doc_x, &doc_y))
    return;

  /* Double-click → select word; triple-click → select line. The click
   * is treated as authoritative — if the user double-clicks, the intent
   * is text selection, not link navigation, even if a link sits under
   * the cursor. */
  if (n_press == 2 || n_press == 3) {
    if (!self->document)
      return;

    FwSelectGranularity gran = (n_press == 2) ? FW_SELECT_WORD : FW_SELECT_LINE;
    double x0, y0, x1, y1;
    if (!fw_document_select_at (self->document, page, doc_x, doc_y, gran,
                                 &x0, &y0, &x1, &y1))
      return;

    /* Claim so the drag gesture doesn't fight us */
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    self->sel_page  = page;
    self->sel_x0    = x0;
    self->sel_y0    = y0;
    self->sel_x1    = x1;
    self->sel_y1    = y1;
    self->selecting = FALSE;

    g_clear_pointer (&self->selected_text, g_free);
    self->selected_text = fw_document_get_text (self->document, page,
                                                 x0, y0, x1, y1);

    if (self->sel_quads) {
      g_array_unref (self->sel_quads);
      self->sel_quads = NULL;
    }
    self->sel_quads = fw_document_get_selection_quads (self->document, page,
                                                       x0, y0, x1, y1);

    FW_TRACE_VIEW ("snap-select: page=%d gran=%s rect=(%.1f,%.1f)-(%.1f,%.1f) text=%s",
                    page, gran == FW_SELECT_WORD ? "word" : "line",
                    x0, y0, x1, y1,
                    self->selected_text ? "yes" : "no");

    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  /* Single click — fall through to link-hit-test (existing behaviour). */
  FwLink *link = fw_view_hit_test_link (self, page, doc_x, doc_y);
  if (!link)
    return;

  /* Claim the gesture so the drag gesture doesn't start text selection */
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  if (link->type == FW_LINK_INTERNAL) {
    FW_TRACE_VIEW ("link click: internal → page %d", link->dest_page);
    g_signal_emit (self, signals[SIGNAL_PAGE_JUMPED], 0, link->dest_page);
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
  if (self->sel_quads) {
    g_array_unref (self->sel_quads);
    self->sel_quads = NULL;
  }

  FW_TRACE_VIEW ("set_document: pages=%d", self->page_count);
  recompute_layout (self);
  update_cache_priority (self);
}

void
fw_view_set_zoom (FwView *self, double zoom)
{
  g_return_if_fail (FW_IS_VIEW (self));
  FW_TRACE_VIEW ("set_zoom: %.2f → %.2f", self->zoom, zoom);

  /* Preserve scroll position across zoom changes on both axes.
   *
   * Vertical anchor: page index + intra-page fraction. Same as before.
   *
   * Horizontal anchor: fraction within the *current page* of where
   * the viewport's horizontal center sits. Anchoring to the page (not
   * the canvas) is what makes zoom-in feel natural on mixed-width
   * docs — at fit-width on a comic with a wider centerfold, the
   * normal page fills the viewport and the canvas is wider than the
   * page; "fraction of canvas" would put the viewport-center anchor
   * at 33% (because canvas extends past the page) whereas
   * "fraction of current page" correctly identifies the user as
   * looking at the page's center (50%). After zoom, the same
   * page-fraction should still be at viewport center. */
  int anchor_page = -1;
  double anchor_frac_y = 0;
  double anchor_frac_x = 0.5;
  gboolean have_x_anchor = FALSE;
  int widget_w = gtk_widget_get_width (GTK_WIDGET (self));

  if (self->vadjustment && self->page_y_offsets && self->page_count > 0) {
    double scroll_y = gtk_adjustment_get_value (self->vadjustment);
    anchor_page = fw_view_get_current_page (self);
    if (anchor_page >= 0 && anchor_page < self->page_count &&
        self->page_heights[anchor_page] > 0) {
      anchor_frac_y = (scroll_y - self->page_y_offsets[anchor_page])
                    / self->page_heights[anchor_page];
      if (anchor_frac_y < 0) anchor_frac_y = 0;
      if (anchor_frac_y > 1) anchor_frac_y = 1;
    }
  }
  if (anchor_page >= 0 && self->hadjustment && widget_w > 0
      && self->page_widths[anchor_page] > 0) {
    /* Compute the page's pre-zoom widget X position via the same math
     * the snapshot loop uses, then derive the page-fraction at
     * viewport center. */
    double scroll_x_old = gtk_adjustment_get_value (self->hadjustment);
    double pw_old = self->page_widths[anchor_page];
    double page_x_widget;
    if (pw_old <= widget_w)
      page_x_widget = (widget_w - pw_old) / 2.0;
    else
      page_x_widget = (self->max_width - pw_old) / 2.0 - scroll_x_old;
    anchor_frac_x = ((double) widget_w / 2.0 - page_x_widget) / pw_old;
    if (anchor_frac_x < 0) anchor_frac_x = 0;
    if (anchor_frac_x > 1) anchor_frac_x = 1;
    have_x_anchor = TRUE;
  }

  self->zoom = zoom;
  recompute_layout (self);

  if (anchor_page >= 0 && self->vadjustment && self->page_y_offsets) {
    double new_val = self->page_y_offsets[anchor_page]
                   + anchor_frac_y * self->page_heights[anchor_page];
    FW_TRACE_VIEW ("scroll restore: page=%d frac_y=%.3f frac_x=%.3f",
                    anchor_page, anchor_frac_y, anchor_frac_x);
    gtk_adjustment_set_value (self->vadjustment, new_val);
  }
  if (have_x_anchor && self->hadjustment && anchor_page >= 0) {
    /* Find the scroll_x that makes the same page-fraction land at
     * viewport center after the new layout. Inverts the snapshot
     * centering math. */
    double pw_new = self->page_widths[anchor_page];
    double upper = gtk_adjustment_get_upper (self->hadjustment);
    double size  = gtk_adjustment_get_page_size (self->hadjustment);
    if (pw_new <= widget_w) {
      /* Page now fits in viewport — center; horizontal scroll is
       * irrelevant (range is zero). */
      gtk_adjustment_set_value (self->hadjustment, 0);
    } else {
      double desired_page_x =
        (double) widget_w / 2.0 - anchor_frac_x * pw_new;
      double new_x = (self->max_width - pw_new) / 2.0 - desired_page_x;
      if (new_x > upper - size) new_x = upper - size;
      if (new_x < 0) new_x = 0;
      gtk_adjustment_set_value (self->hadjustment, new_x);
    }
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

  /* Fit the *current* visible page rather than the widest page in the
   * document. Picking the widest works fine for technical PDFs where
   * every page is the same size, but on a comic CBZ with even one
   * centerfold spread (~2x normal width) it makes every normal page
   * render at ~35% — there's empty viewport on either side and the
   * user has to manually zoom in. With current-page sizing, normal
   * pages fill the viewport; landing on a wider page is then
   * horizontally scrollable until the user hits Ctrl+1 again.
   *
   * page_y_offsets is built by recompute_layout; if it's not ready
   * yet (initial open before allocation), fall back to page 0. */
  int page = 0;
  if (self->page_y_offsets && self->vadjustment) {
    int current = fw_view_get_current_page (self);
    if (current >= 0 && current < self->page_count)
      page = current;
  }

  double w, h;
  fw_document_get_page_size (self->document, page, &w, &h);
  if (self->rotation == 90 || self->rotation == 270) {
    double tmp = w; w = h; h = tmp;
  }
  if (w <= 0)
    return 1.0;

  return (double) viewport_w / w;
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

  /* Find the page whose top is closest to (but not past) the viewport top.
   * In facing-pages mode both pages of a pair share a y_offset; we
   * report the lower-numbered page as the current one (so the header
   * label tracks the same page the user thinks of as "current"). */
  for (int i = self->page_count - 1; i >= 0; i--) {
    if (self->page_y_offsets[i] <= scroll_y + 1.0)
      return view_pair_first (self, i);
  }
  return 0;
}

double
fw_view_get_current_row_width (FwView *self)
{
  g_return_val_if_fail (FW_IS_VIEW (self), 0);
  if (!self->page_widths || self->page_count == 0)
    return 0;

  int p = fw_view_get_current_page (self);
  if (p < 0 || p >= self->page_count)
    return 0;

  if (view_page_is_paired (self, p)) {
    int first  = view_pair_first (self, p);
    int second = first + 1;
    if (second < self->page_count)
      return self->page_widths[first] + PAGE_GAP + self->page_widths[second];
  }
  return self->page_widths[p];
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

/* ── Search wiring ───────────────────────────────────────────────── */

static void
on_search_changed (FwSearch *search, gpointer user_data)
{
  (void) search;
  FwView *self = FW_VIEW (user_data);
  if (!self->redraw_pending) {
    self->redraw_pending = TRUE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

static void
on_search_current_changed (FwSearch *search, gpointer user_data)
{
  (void) search;
  FwView *self = FW_VIEW (user_data);
  fw_view_reveal_active_hit (self);
  if (!self->redraw_pending) {
    self->redraw_pending = TRUE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

void
fw_view_set_search (FwView *self, FwSearch *search)
{
  g_return_if_fail (FW_IS_VIEW (self));

  if (self->search == search)
    return;

  if (self->search) {
    if (self->search_hits_handler)
      g_signal_handler_disconnect (self->search, self->search_hits_handler);
    if (self->search_current_handler)
      g_signal_handler_disconnect (self->search, self->search_current_handler);
    self->search_hits_handler = 0;
    self->search_current_handler = 0;
  }

  g_set_object (&self->search, search);

  if (self->search) {
    self->search_hits_handler = g_signal_connect (
      self->search, "hits-changed",
      G_CALLBACK (on_search_changed), self);
    self->search_current_handler = g_signal_connect (
      self->search, "current-changed",
      G_CALLBACK (on_search_current_changed), self);
  }

  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
fw_view_reveal_active_hit (FwView *self)
{
  g_return_if_fail (FW_IS_VIEW (self));

  if (!self->search || !self->vadjustment || !self->page_y_offsets)
    return;

  int idx = fw_search_active_index (self->search);
  if (idx < 0)
    return;

  int n_hits = 0;
  const FwSearchHit *hits = fw_search_peek_hits (self->search, &n_hits);
  if (!hits || idx >= n_hits)
    return;

  const FwSearchHit *hit = &hits[idx];
  if (hit->page < 0 || hit->page >= self->page_count)
    return;

  /* Position the hit roughly 1/3 down from the viewport top so the user
   * has reading context above it. */
  int widget_h = gtk_widget_get_height (GTK_WIDGET (self));
  if (widget_h <= 0)
    widget_h = 700;

  double hit_y = self->page_y_offsets[hit->page] + hit->y0 * self->zoom;
  double target = hit_y - widget_h / 3.0;

  double upper = gtk_adjustment_get_upper (self->vadjustment);
  double page_size = gtk_adjustment_get_page_size (self->vadjustment);
  if (target < 0) target = 0;
  if (target > upper - page_size) target = upper - page_size;
  if (target < 0) target = 0;

  gtk_adjustment_set_value (self->vadjustment, target);

  /* Horizontal: also reveal if the hit is offscreen horizontally
   * (zoomed past fit-width). */
  if (self->hadjustment && self->page_widths) {
    int widget_w = gtk_widget_get_width (GTK_WIDGET (self));
    if (widget_w <= 0) widget_w = 900;

    double pw = self->page_widths[hit->page];
    double page_x;
    if (self->max_width <= widget_w)
      page_x = (widget_w - pw) / 2.0;
    else
      page_x = (self->max_width - pw) / 2.0;

    double hit_x = page_x + hit->x0 * self->zoom;
    double hadj_val = gtk_adjustment_get_value (self->hadjustment);
    double hadj_size = gtk_adjustment_get_page_size (self->hadjustment);
    if (hit_x < hadj_val || hit_x > hadj_val + hadj_size) {
      double hadj_upper = gtk_adjustment_get_upper (self->hadjustment);
      double new_h = hit_x - hadj_size / 3.0;
      if (new_h < 0) new_h = 0;
      if (new_h > hadj_upper - hadj_size) new_h = hadj_upper - hadj_size;
      if (new_h < 0) new_h = 0;
      gtk_adjustment_set_value (self->hadjustment, new_h);
    }
  }
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_view_dispose (GObject *object)
{
  FwView *self = FW_VIEW (object);

  fw_view_free_link_cache (self);
  if (self->search) {
    if (self->search_hits_handler)
      g_signal_handler_disconnect (self->search, self->search_hits_handler);
    if (self->search_current_handler)
      g_signal_handler_disconnect (self->search, self->search_current_handler);
    self->search_hits_handler = 0;
    self->search_current_handler = 0;
  }
  g_clear_object (&self->search);

  if (self->settings) {
    if (self->ruler_changed_handler)
      g_signal_handler_disconnect (self->settings, self->ruler_changed_handler);
    if (self->loupe_changed_handler)
      g_signal_handler_disconnect (self->settings, self->loupe_changed_handler);
    if (self->crop_changed_handler)
      g_signal_handler_disconnect (self->settings, self->crop_changed_handler);
    if (self->manga_changed_handler)
      g_signal_handler_disconnect (self->settings, self->manga_changed_handler);
    if (self->webtoon_changed_handler)
      g_signal_handler_disconnect (self->settings, self->webtoon_changed_handler);
    if (self->facing_changed_handler)
      g_signal_handler_disconnect (self->settings, self->facing_changed_handler);
    self->ruler_changed_handler = 0;
    self->loupe_changed_handler = 0;
    self->crop_changed_handler = 0;
    self->manga_changed_handler = 0;
    self->webtoon_changed_handler = 0;
    self->facing_changed_handler = 0;
    g_clear_object (&self->settings);
  }
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
  g_free (self->pair_partner);
  g_free (self->selected_text);
  if (self->sel_quads)
    g_array_unref (self->sel_quads);
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

  /* Emitted when the user activates an internal link. The window listens
   * to push the previous viewport into navigation history. Plain scroll
   * and search-hit-reveal do NOT emit this signal — only explicit jumps. */
  signals[SIGNAL_PAGE_JUMPED] = g_signal_new (
    "page-jumped", FW_TYPE_VIEW,
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
on_ruler_setting_changed (GSettings *settings, const char *key, gpointer user_data)
{
  (void) key;
  FwView *self = user_data;
  self->reading_ruler = g_settings_get_boolean (settings, "reading-ruler");
  FW_TRACE_VIEW ("reading-ruler=%d", self->reading_ruler);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_loupe_setting_changed (GSettings *settings, const char *key, gpointer user_data)
{
  (void) key;
  FwView *self = user_data;
  self->loupe = g_settings_get_boolean (settings, "loupe");
  FW_TRACE_VIEW ("loupe=%d", self->loupe);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Common path for the three layout-affecting comic-mode toggles
 * (manga, webtoon, facing-pages). Each one changes total_height /
 * y_offsets so we capture the (page, intra-page-fraction) anchor
 * before re-laying-out and restore it after — same pattern as
 * fw_view_set_zoom and on_crop_setting_changed. */
static void
view_apply_layout_change (FwView *self)
{
  int anchor_page = -1;
  double anchor_frac_y = 0;
  if (self->vadjustment && self->page_y_offsets && self->page_count > 0) {
    double scroll_y = gtk_adjustment_get_value (self->vadjustment);
    anchor_page = fw_view_get_current_page (self);
    if (anchor_page >= 0 && anchor_page < self->page_count
        && self->page_heights[anchor_page] > 0) {
      anchor_frac_y = (scroll_y - self->page_y_offsets[anchor_page])
                    / self->page_heights[anchor_page];
      if (anchor_frac_y < 0) anchor_frac_y = 0;
      if (anchor_frac_y > 1) anchor_frac_y = 1;
    }
  }

  recompute_layout (self);

  if (anchor_page >= 0 && self->vadjustment && self->page_y_offsets) {
    double new_val = self->page_y_offsets[anchor_page]
                   + anchor_frac_y * self->page_heights[anchor_page];
    gtk_adjustment_set_value (self->vadjustment, new_val);
  }

  update_cache_priority (self);
}

static void
on_manga_setting_changed (GSettings *settings, const char *key, gpointer user_data)
{
  (void) key;
  FwView *self = user_data;
  self->manga_mode = g_settings_get_boolean (settings, "manga-mode");
  FW_TRACE_VIEW ("manga-mode=%d", self->manga_mode);
  /* Manga only flips left/right within paired layouts — pure scroll
   * geometry doesn't change unless facing-pages is also on. Cheap
   * redraw is enough; layout-anchor preservation isn't needed. */
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_webtoon_setting_changed (GSettings *settings, const char *key, gpointer user_data)
{
  (void) key;
  FwView *self = user_data;
  self->webtoon_mode = g_settings_get_boolean (settings, "webtoon-mode");
  FW_TRACE_VIEW ("webtoon-mode=%d", self->webtoon_mode);
  view_apply_layout_change (self);
}

static void
on_facing_setting_changed (GSettings *settings, const char *key, gpointer user_data)
{
  (void) key;
  FwView *self = user_data;
  self->facing_pages = g_settings_get_boolean (settings, "facing-pages");
  FW_TRACE_VIEW ("facing-pages=%d", self->facing_pages);
  view_apply_layout_change (self);
}

static void
on_crop_setting_changed (GSettings *settings, const char *key, gpointer user_data)
{
  (void) key;
  FwView *self = user_data;
  gboolean enabled = g_settings_get_boolean (settings, "crop-margins");
  self->crop_margins = enabled;

  /* Capture (page, intra-page-fraction) anchor before the layout
   * changes — pages get shorter when crop turns on (or longer when it
   * turns off), and the same scroll_y would otherwise point to a
   * different page. Same pattern fw_view_set_zoom uses for zoom. */
  int anchor_page = -1;
  double anchor_frac_y = 0;
  if (self->vadjustment && self->page_y_offsets && self->page_count > 0) {
    double scroll_y = gtk_adjustment_get_value (self->vadjustment);
    anchor_page = fw_view_get_current_page (self);
    if (anchor_page >= 0 && anchor_page < self->page_count
        && self->page_heights[anchor_page] > 0) {
      anchor_frac_y = (scroll_y - self->page_y_offsets[anchor_page])
                    / self->page_heights[anchor_page];
      if (anchor_frac_y < 0) anchor_frac_y = 0;
      if (anchor_frac_y > 1) anchor_frac_y = 1;
    }
  }

  if (enabled && self->document) {
    /* Snapshot the current visible page's content bbox; convert to
     * fractional margins relative to the full page size. Applied
     * uniformly to every page (assumption: most docs have consistent
     * margins across pages). If the backend doesn't support stext
     * (DjVu, CBR) or the page has no text, leave fractions invalid
     * — the snapshot/layout paths fall back to full-page rendering. */
    int probe_page = self->page_count > 0
      ? fw_view_get_current_page (self) : -1;
    if (probe_page < 0 || probe_page >= self->page_count)
      probe_page = 0;

    double bx0, by0, bx1, by1;
    if (fw_document_get_content_bbox (self->document, probe_page,
                                       &bx0, &by0, &bx1, &by1)) {
      double pw, ph;
      fw_document_get_page_size (self->document, probe_page, &pw, &ph);
      if (self->rotation == 90 || self->rotation == 270) {
        double tmp = pw; pw = ph; ph = tmp;
      }
      if (pw > 0 && ph > 0) {
        self->crop_l_frac = bx0 / pw;
        self->crop_r_frac = (pw - bx1) / pw;
        self->crop_t_frac = by0 / ph;
        self->crop_b_frac = (ph - by1) / ph;
        if (self->crop_l_frac < 0) self->crop_l_frac = 0;
        if (self->crop_r_frac < 0) self->crop_r_frac = 0;
        if (self->crop_t_frac < 0) self->crop_t_frac = 0;
        if (self->crop_b_frac < 0) self->crop_b_frac = 0;
        self->crop_fractions_valid = TRUE;
        FW_TRACE_VIEW ("crop-margins on: l=%.3f r=%.3f t=%.3f b=%.3f",
                        self->crop_l_frac, self->crop_r_frac,
                        self->crop_t_frac, self->crop_b_frac);
      }
    } else {
      self->crop_fractions_valid = FALSE;
      FW_TRACE_VIEW ("crop-margins on: backend has no content bbox");
    }
  } else {
    self->crop_fractions_valid = FALSE;
    FW_TRACE_VIEW ("crop-margins off");
  }

  recompute_layout (self);

  /* Restore the (page, frac) anchor after the layout has applied the
   * new crop fractions. */
  if (anchor_page >= 0 && self->vadjustment && self->page_y_offsets) {
    double new_val = self->page_y_offsets[anchor_page]
                   + anchor_frac_y * self->page_heights[anchor_page];
    gtk_adjustment_set_value (self->vadjustment, new_val);
  }

  update_cache_priority (self);
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

  /* Settings — reading-ruler, loupe, crop-margins, and the Phase 13
   * comic modes (manga / webtoon / facing-pages) all toggle live;
   * kinetic-scrolling is owned by FwWindow. */
  self->settings = g_settings_new (APP_ID);
  self->reading_ruler = g_settings_get_boolean (self->settings,
                                                 "reading-ruler");
  self->loupe = g_settings_get_boolean (self->settings, "loupe");
  self->crop_margins = g_settings_get_boolean (self->settings, "crop-margins");
  self->manga_mode    = g_settings_get_boolean (self->settings, "manga-mode");
  self->webtoon_mode  = g_settings_get_boolean (self->settings, "webtoon-mode");
  self->facing_pages  = g_settings_get_boolean (self->settings, "facing-pages");
  self->ruler_changed_handler = g_signal_connect (
    self->settings, "changed::reading-ruler",
    G_CALLBACK (on_ruler_setting_changed), self);
  self->loupe_changed_handler = g_signal_connect (
    self->settings, "changed::loupe",
    G_CALLBACK (on_loupe_setting_changed), self);
  self->crop_changed_handler = g_signal_connect (
    self->settings, "changed::crop-margins",
    G_CALLBACK (on_crop_setting_changed), self);
  self->manga_changed_handler = g_signal_connect (
    self->settings, "changed::manga-mode",
    G_CALLBACK (on_manga_setting_changed), self);
  self->webtoon_changed_handler = g_signal_connect (
    self->settings, "changed::webtoon-mode",
    G_CALLBACK (on_webtoon_setting_changed), self);
  self->facing_changed_handler = g_signal_connect (
    self->settings, "changed::facing-pages",
    G_CALLBACK (on_facing_setting_changed), self);

  /* Scroll handling is left to GtkScrolledWindow's native path — that
   * way diagonal trackpad motion (which Wayland delivers as paired
   * dx/dy events) is handled the same way every other GTK app handles
   * it. The kinetic-scrolling setting flows from the window into
   * gtk_scrolled_window_set_kinetic_scrolling() at the parent.
   *
   * The earlier per-event cap (intended to throttle wheel flicks so
   * the render cache could keep up) is obsolete: the v0.14 GThreadPool
   * sort-function priority dispatch and v0.17 fz_cookie mid-render
   * abort already guarantee the cache responds to scroll velocity
   * without a hard input-side cap. */

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
