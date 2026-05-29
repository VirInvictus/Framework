/* fw-reflow-document-md.h — Markdown reflow backend (Phase 17.x)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fw-reflow-document.h"

G_BEGIN_DECLS

#define FW_TYPE_REFLOW_DOCUMENT_MD (fw_reflow_document_md_get_type ())

G_DECLARE_FINAL_TYPE (FwReflowDocumentMd, fw_reflow_document_md,
                      FW, REFLOW_DOCUMENT_MD, GObject)

FwReflowDocumentMd *fw_reflow_document_md_new (void);

G_END_DECLS
