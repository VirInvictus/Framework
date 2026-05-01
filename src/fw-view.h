/* fw-view.h — Custom page rendering widget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "fw-document.h"
#include "fw-cache.h"
#include "fw-search.h"

G_BEGIN_DECLS

#define FW_TYPE_VIEW (fw_view_get_type ())

G_DECLARE_FINAL_TYPE (FwView, fw_view, FW, VIEW, GtkWidget)

FwView *fw_view_new          (void);
void    fw_view_set_document (FwView     *self,
                              FwDocument *document,
                              FwCache    *cache);
void    fw_view_set_zoom     (FwView     *self,
                              double      zoom);
double  fw_view_fit_width_zoom (FwView   *self,
                              int       viewport_w);
double  fw_view_fit_page_zoom (FwView   *self,
                              int       viewport_w,
                              int       viewport_h);
void    fw_view_go_to_page   (FwView     *self,
                              int         page);
int     fw_view_get_current_page (FwView *self);
/* Return the displayed pixel width of the current row — the active
 * page if standalone, or the paired-page pair width (incl. gutter) if
 * facing-pages is on and the current page is paired. Used by the
 * window to auto-grow when a wide spread becomes current. */
double  fw_view_get_current_row_width (FwView *self);
void    fw_view_set_invert     (FwView     *self,
                                gboolean    invert);
void    fw_view_set_rotation   (FwView     *self,
                                int         rotation);
const char *fw_view_get_selected_text (FwView *self);

void    fw_view_set_search        (FwView    *self,
                                   FwSearch  *search);
/* Scroll the viewport so the active search hit is centred. */
void    fw_view_reveal_active_hit (FwView    *self);

G_END_DECLS
