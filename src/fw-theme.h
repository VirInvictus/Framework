/* fw-theme.h — Owned dark/light theming (libadwaita replacement).
 *
 * A singleton GObject carrying a `dark` boolean property, resolved from
 * the freedesktop `org.freedesktop.portal.Settings` color-scheme
 * preference over D-Bus and kept live via `SettingChanged`.  Replaces
 * the AdwStyleManager dependency dropped at v0.80.0.  Also owns the two
 * GtkCssProviders (the swapped Kanagawa palette + the static structural
 * stylesheet) that give the app its look now that the adwaita sheet is
 * gone.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define FW_TYPE_THEME (fw_theme_get_type ())

G_DECLARE_FINAL_TYPE (FwTheme, fw_theme, FW, THEME, GObject)

/* Install the CSS providers, apply the current color scheme, and
 * subscribe to live changes.  Call once, after a GdkDisplay exists
 * (i.e. from the application's startup, after gtk_init).  Idempotent. */
void      fw_theme_install (void);

/* The process-wide theme singleton.  Connect to its "notify::dark" to
 * react to light/dark flips, the direct replacement for the old
 * AdwStyleManager notify::dark subscription. */
FwTheme  *fw_theme_get_default (void);

/* TRUE when the resolved color scheme is dark (also the default when no
 * portal backend answers). */
gboolean  fw_theme_get_dark (FwTheme *self);

G_END_DECLS
