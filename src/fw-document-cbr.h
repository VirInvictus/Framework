/* fw-document-cbr.h — Comic Book RAR (CBR) backend via libarchive
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include "fw-document.h"

G_BEGIN_DECLS

#define FW_TYPE_DOCUMENT_CBR (fw_document_cbr_get_type ())

G_DECLARE_FINAL_TYPE (FwDocumentCbr, fw_document_cbr, FW, DOCUMENT_CBR, GObject)

FwDocumentCbr *fw_document_cbr_new (void);

G_END_DECLS
