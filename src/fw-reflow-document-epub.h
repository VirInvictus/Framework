/* fw-reflow-document-epub.h — EPUB reflow backend (Phase 13.1 Phase 3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fw-reflow-document.h"

G_BEGIN_DECLS

#define FW_TYPE_REFLOW_DOCUMENT_EPUB (fw_reflow_document_epub_get_type ())

G_DECLARE_FINAL_TYPE (FwReflowDocumentEpub, fw_reflow_document_epub,
                      FW, REFLOW_DOCUMENT_EPUB, GObject)

FwReflowDocumentEpub *fw_reflow_document_epub_new (void);

G_END_DECLS
