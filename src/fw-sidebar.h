/* fw-sidebar.h — TOC sidebar
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "fw-document.h"

G_BEGIN_DECLS

#define FW_TYPE_SIDEBAR (fw_sidebar_get_type ())

G_DECLARE_FINAL_TYPE (FwSidebar, fw_sidebar, FW, SIDEBAR, GtkWidget)

FwSidebar *fw_sidebar_new     (void);
void       fw_sidebar_set_toc (FwSidebar       *self,
                               const FwTocNode *toc);

/* Highlight the TOC entry whose destination page is the deepest one ≤
 * current_page. Scrolls the tree view so the row is visible. No-op if
 * the document has no TOC. */
void       fw_sidebar_set_current_page (FwSidebar *self, int current_page);

/* Signal: "page-requested" — emitted when user clicks a TOC entry.
 * Handler signature: void handler (FwSidebar *sidebar, int page, gpointer data); */

G_END_DECLS
