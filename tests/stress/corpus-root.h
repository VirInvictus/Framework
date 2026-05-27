/* tests/stress/corpus-root.h — shared corpus-root resolution
 *
 * The stress/bench targets resolve their sample documents against a
 * single root so the suite is reproducible (see tests/README.md). This
 * header is the one definition of that policy; each target builds full
 * paths as root / "filename".
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

/* Corpus root, in priority order:
 *   1. $FW_TEST_CORPUS_ROOT       (explicit override)
 *   2. $G_TEST_SRCDIR/.testfiles  (meson passes the project source root)
 *   3. ./.testfiles               (bare run from the repo root)
 * Caller frees with g_free. */
static inline char *
fw_test_corpus_root (void)
{
  const char *env = g_getenv ("FW_TEST_CORPUS_ROOT");
  if (env && env[0])
    return g_strdup (env);
  const char *srcdir = g_getenv ("G_TEST_SRCDIR");
  if (srcdir && srcdir[0])
    return g_build_filename (srcdir, ".testfiles", NULL);
  return g_strdup (".testfiles");
}
