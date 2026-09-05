/* fw-dialog-widgets.h — Owned dialog + boxed-list row helpers
 *
 * The libadwaita-free stand-ins for AdwDialog/AdwToolbarView/
 * AdwPreferences*: a modal transient GtkWindow with a flat header and
 * Escape-to-close, and GtkListBox "boxed-list" rows whose padding and
 * typography come from the owned stylesheet. Ported from the Hermitage
 * sibling's widgets.py (v0.80.0); extracted out of fw-window.c so the
 * window file stays navigable.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Build a modal dialog window; returns the window and hands back the
 * vertical content box (inside a scroller) to fill. */
GtkWindow  *fw_dialog_new   (GtkWindow   *parent,
                             const char  *title,
                             int          w,
                             int          h,
                             GtkBox     **out_content);

/* Append a titled group (optional description) to `page` and return its
 * boxed list for rows. */
GtkListBox *fw_pref_group   (GtkBox      *page,
                             const char  *title,
                             const char  *desc);

/* A row: title (+ optional subtitle) on the left, a control on the
 * right. `suffix` may be NULL (a plain value/label row). */
GtkWidget  *fw_value_row    (const char  *title,
                             const char  *subtitle,
                             GtkWidget   *suffix);

G_END_DECLS
