/* fw-fonts.c — Register bundled fonts with FontConfig.
 *
 * Three OFL-licensed families ship in `data/fonts/`:
 *   * OpenDyslexic         (~850 KB, accessibility)
 *   * Atkinson Hyperlegible (~220 KB, default high-readability sans)
 *   * Crimson Pro           (~590 KB, body serif option)
 *
 * Installed to ${datadir}/framework/fonts/<Family>/, registered here
 * via FcConfigAppFontAddDir so Pango discovers them without the user
 * having to install anything system-wide. Atomic to call multiple
 * times — FcConfigAppFontAddDir is idempotent on already-registered
 * directories.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-config.h"
#include "fw-fonts.h"
#include "fw-debug.h"

#include <fontconfig/fontconfig.h>

static gboolean
add_dir_if_present (FcConfig *cfg, const char *path, const char *family)
{
  g_autofree char *full = g_build_filename (path, family, NULL);
  if (!g_file_test (full, G_FILE_TEST_IS_DIR))
    return FALSE;

  if (FcConfigAppFontAddDir (cfg, (const FcChar8 *) full)) {
    FW_TRACE_WINDOW ("fonts: registered '%s'", full);
    return TRUE;
  }
  g_warning ("fonts: FcConfigAppFontAddDir failed for '%s'", full);
  return FALSE;
}

static gboolean
register_root (FcConfig *cfg, const char *root)
{
  if (!root || !*root || !g_file_test (root, G_FILE_TEST_IS_DIR))
    return FALSE;

  /* The known bundle: three families, one subdir each. Registering
   * the parent root with a single FcConfigAppFontAddDir would also
   * pick them up because that function recurses, but adding each
   * family individually keeps the trace output legible and survives
   * unfamiliar siblings (e.g. README) being added later. */
  gboolean any = FALSE;
  any |= add_dir_if_present (cfg, root, "OpenDyslexic");
  any |= add_dir_if_present (cfg, root, "AtkinsonHyperlegible");
  any |= add_dir_if_present (cfg, root, "CrimsonPro");
  return any;
}

void
fw_fonts_register (void)
{
  FcConfig *cfg = FcConfigGetCurrent ();
  if (!cfg) {
    g_warning ("fonts: no FontConfig config — cannot register bundled fonts");
    return;
  }

  /* Probe sources in priority order. Stop as soon as one yields a
   * registration so a dev run doesn't pull in stale system copies. */
  const char *env_override = g_getenv ("FW_FONT_DIR");
  if (env_override && register_root (cfg, env_override))
    return;

  const char *xdg_data = g_getenv ("FRAMEWORK_DATADIR");
  if (xdg_data) {
    g_autofree char *p = g_build_filename (xdg_data, "framework", "fonts", NULL);
    if (register_root (cfg, p))
      return;
  }

  /* System install (set by meson). */
  {
    g_autofree char *p = g_build_filename (FW_DATADIR, "framework", "fonts", NULL);
    if (register_root (cfg, p))
      return;
  }

  /* Source-root fallback for `meson devenv` / direct ./builddir runs. */
  {
    g_autofree char *p = g_build_filename (FW_SOURCE_ROOT, "data", "fonts", NULL);
    if (register_root (cfg, p))
      return;
  }

  g_warning ("fonts: no bundled-font directory found "
             "(checked FW_FONT_DIR, $FRAMEWORK_DATADIR, %s, %s)",
             FW_DATADIR "/framework/fonts",
             FW_SOURCE_ROOT "/data/fonts");
}
