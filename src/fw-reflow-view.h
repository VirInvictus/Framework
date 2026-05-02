/* fw-reflow-view.h — GtkListView-backed reflow widget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "fw-reflow-document.h"

G_BEGIN_DECLS

#define FW_TYPE_REFLOW_VIEW (fw_reflow_view_get_type ())

G_DECLARE_FINAL_TYPE (FwReflowView, fw_reflow_view, FW, REFLOW_VIEW, GtkWidget)

FwReflowView *fw_reflow_view_new           (void);
void          fw_reflow_view_set_document  (FwReflowView     *self,
                                            FwReflowDocument *doc);

/* Scroll the view so the block carrying `anchor` is visible. No-op when
 * the active document doesn't recognise the anchor. */
void          fw_reflow_view_scroll_to_anchor (FwReflowView *self,
                                               const char   *anchor);

/* Page-by-page navigation. `direction` +1 advances to the next page,
 * -1 to the previous, 0 to the first, G_MAXINT to the last. Pages are
 * computed by walking every block once at the active viewport size
 * and breaking when measured heights exceed the viewport — never
 * splitting a block across pages. */
void          fw_reflow_view_scroll_by_page   (FwReflowView *self,
                                               int           direction);

/* Current page index (0-based) and total page count. Both reflect the
 * most recent pagination pass — they change on viewport resize and
 * font-preference change. The "page-changed" signal fires whenever
 * either value changes; subscribe to refresh the UI. */
guint         fw_reflow_view_get_current_page (FwReflowView *self);
guint         fw_reflow_view_get_total_pages  (FwReflowView *self);

/* Signal:
 *   "page-changed" (uint current, uint total) — emitted after pagination
 *   recomputes or after fw_reflow_view_scroll_by_page changes pages. */

G_END_DECLS
