/* fw-cache.h — Pre-cache engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fw-document.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define FW_TYPE_CACHE (fw_cache_get_type ())

G_DECLARE_FINAL_TYPE (FwCache, fw_cache, FW, CACHE, GObject)

FwCache         *fw_cache_new           (FwDocument      *document,
                                         GtkWidget       *view_widget);

void             fw_cache_start         (FwCache         *self,
                                         double           zoom,
                                         int              rotation);

void             fw_cache_invalidate_all (FwCache        *self);
void             fw_cache_invalidate_page (FwCache       *self,
                                           int            page);

void             fw_cache_set_priority  (FwCache         *self,
                                         const int       *visible_pages,
                                         int              n_visible);

gboolean         fw_cache_set_velocity  (FwCache         *self,
                                         double           velocity);

cairo_surface_t *fw_cache_get_page      (FwCache         *self,
                                         int              page);

gboolean         fw_cache_page_ready    (FwCache         *self,
                                         int              page);

/* Get cached GdkTexture for a page — reused across frames so we don't
 * allocate a new texture wrapper every snapshot(). Returns a borrowed
 * reference (do not unref). Falls back to the closest-zoom previous
 * snapshot when the current-gen texture isn't ready (try_closest
 * pattern from Sumatra/Sioyek). Returns NULL when nothing is cached. */
GdkTexture      *fw_cache_get_texture    (FwCache         *self,
                                          int              page);

/* Get a persistent low-resolution thumbnail for the given page. Thumbnails
 * are rendered lazily at fixed size and kept for the life of the cache —
 * they serve as always-available placeholders during fast scroll. */
GdkTexture      *fw_cache_get_thumbnail  (FwCache         *self,
                                          int              page,
                                          double           page_w,
                                          double           page_h);

void             fw_cache_stop          (FwCache         *self);

void             fw_cache_set_scale_factor (FwCache      *self,
                                            int           scale_factor);

G_END_DECLS
