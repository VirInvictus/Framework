/* fw-document-pdf.h — MuPDF backend
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fw-document.h"

G_BEGIN_DECLS

#define FW_TYPE_DOCUMENT_PDF (fw_document_pdf_get_type ())

G_DECLARE_FINAL_TYPE (FwDocumentPdf, fw_document_pdf, FW, DOCUMENT_PDF, GObject)

FwDocumentPdf *fw_document_pdf_new (void);

G_END_DECLS
