/* fw-reflow-document-fb2.h — FB2 reflow backend (Phase 13.1 Phase 2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fw-reflow-document.h"

G_BEGIN_DECLS

#define FW_TYPE_REFLOW_DOCUMENT_FB2 (fw_reflow_document_fb2_get_type ())

G_DECLARE_FINAL_TYPE (FwReflowDocumentFb2, fw_reflow_document_fb2,
                      FW, REFLOW_DOCUMENT_FB2, GObject)

FwReflowDocumentFb2 *fw_reflow_document_fb2_new (void);

G_END_DECLS
