/* fw-window.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "fw-application.h"

G_BEGIN_DECLS

#define FW_TYPE_WINDOW (fw_window_get_type ())

G_DECLARE_FINAL_TYPE (FwWindow, fw_window, FW, WINDOW, AdwApplicationWindow)

FwWindow *fw_window_new       (FwApplication *app);
void      fw_window_open_file (FwWindow      *self,
                               const char    *path);

G_END_DECLS
