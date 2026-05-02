/* fw-reflow-document-txt.h — TXT reflow backend (Phase 13.1 Phase 1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fw-reflow-document.h"

G_BEGIN_DECLS

#define FW_TYPE_REFLOW_DOCUMENT_TXT (fw_reflow_document_txt_get_type ())

G_DECLARE_FINAL_TYPE (FwReflowDocumentTxt, fw_reflow_document_txt,
                      FW, REFLOW_DOCUMENT_TXT, GObject)

FwReflowDocumentTxt *fw_reflow_document_txt_new (void);

G_END_DECLS
