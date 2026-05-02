/* fw-reflow-sidebar.h — Sidebar for reflow documents
 *
 * Lightweight peer to FwSidebar. Bound to a GListModel<FwReflowTocItem>
 * directly — reflow TOCs are flat (sections don't nest in the model)
 * and the entries carry anchor-id strings rather than page numbers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "fw-reflow-document.h"

G_BEGIN_DECLS

#define FW_TYPE_REFLOW_SIDEBAR (fw_reflow_sidebar_get_type ())

G_DECLARE_FINAL_TYPE (FwReflowSidebar, fw_reflow_sidebar,
                      FW, REFLOW_SIDEBAR, GtkWidget)

FwReflowSidebar *fw_reflow_sidebar_new      (void);
void             fw_reflow_sidebar_set_toc  (FwReflowSidebar *self,
                                             GListModel      *toc_model);

/* Signal: "anchor-requested" — emitted when the user clicks a TOC row.
 * Handler signature:
 *   void handler (FwReflowSidebar *sidebar,
 *                 const char      *anchor_id,
 *                 gpointer         user_data); */

G_END_DECLS
