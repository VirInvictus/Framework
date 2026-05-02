/* fw-fonts.h — Bundled-font registration with FontConfig
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Register every bundled font directory with the global FontConfig
 * config so they're discoverable by Pango (and therefore GTK CSS).
 * Idempotent — safe to call multiple times. Looks for fonts in:
 *   - $FW_FONT_DIR (env override, dev/CI)
 *   - $FRAMEWORK_DATADIR/framework/fonts/      (Flatpak / staged install)
 *   - <prefix>/share/framework/fonts/           (system install — FW_DATADIR)
 *   - <source-root>/data/fonts/                 (uninstalled / dev runs)
 */
void fw_fonts_register (void);

G_END_DECLS
