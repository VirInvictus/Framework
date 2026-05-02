/* fw-reflow-document-mobi.h — MOBI / KF7 reflow backend
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fw-reflow-document.h"

G_BEGIN_DECLS

#define FW_TYPE_REFLOW_DOCUMENT_MOBI (fw_reflow_document_mobi_get_type ())

G_DECLARE_FINAL_TYPE (FwReflowDocumentMobi, fw_reflow_document_mobi,
                      FW, REFLOW_DOCUMENT_MOBI, GObject)

FwReflowDocumentMobi *fw_reflow_document_mobi_new (void);

G_END_DECLS
