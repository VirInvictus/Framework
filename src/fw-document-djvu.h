/* fw-document-djvu.h — DjVuLibre backend
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fw-document.h"

G_BEGIN_DECLS

#define FW_TYPE_DOCUMENT_DJVU (fw_document_djvu_get_type ())

G_DECLARE_FINAL_TYPE (FwDocumentDjvu, fw_document_djvu, FW, DOCUMENT_DJVU, GObject)

FwDocumentDjvu *fw_document_djvu_new (void);

G_END_DECLS
