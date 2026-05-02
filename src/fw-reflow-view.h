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

FwReflowView *fw_reflow_view_new          (void);
void          fw_reflow_view_set_document (FwReflowView     *self,
                                           FwReflowDocument *doc);

G_END_DECLS
