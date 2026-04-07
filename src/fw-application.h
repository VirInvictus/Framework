/* fw-application.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define FW_TYPE_APPLICATION (fw_application_get_type ())

G_DECLARE_FINAL_TYPE (FwApplication, fw_application, FW, APPLICATION, AdwApplication)

FwApplication *fw_application_new (void);

G_END_DECLS
