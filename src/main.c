/* main.c — Entry point for Framework
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-config.h"
#include "fw-application.h"
#include "fw-debug.h"
#include "fw-sandbox.h"

#include <stdio.h>
#include <string.h>

#if FW_STRESS_BUILD
# include "fw-document.h"
# include "fw-cache.h"
# include <gtk/gtk.h>
# include <stdlib.h>

/* `--self-test`: 5-second smoke gate for CI / Flatpak builds.
 *
 * Built only with `-Dstress=true`. Opens a known-good document
 * (argv[2] if given, otherwise a corpus sample), drives the cache
 * through enough work to exercise both the open path and the
 * render-worker pipeline, asserts that page 0 actually renders,
 * exits 0 on pass / 1 on fail. Headless — no widget, no window.
 *
 * The original Phase 12.4 spec called for hashing the rendered
 * surface against a baseline. Hashes are too brittle across MuPDF
 * versions and platform fonts to be useful CI signal — they false-
 * positive on every legitimate upstream update. The smoke gate
 * checks the property that *actually* breaks on toolchain
 * regressions: "the binary opens a document, the cache renders the
 * first page within the timeout, and nothing crashed." */
#define SELF_TEST_TIMEOUT_MS 5000
/* Default doc when --self-test is invoked with no path. Relative to the
 * CWD (run from the repo root); pass an explicit path to override. */
#define SELF_TEST_DEFAULT ".testfiles/effective-java.pdf"

static int
run_self_test (const char *path)
{
  fw_debug_init ();
  gtk_init ();

  fprintf (stderr, "self-test: opening %s\n", path);

  GError *error = NULL;
  FwDocument *doc = fw_document_new_for_path (path, &error);
  if (!doc) {
    fprintf (stderr, "self-test: open failed: %s\n",
             error ? error->message : "(null)");
    g_clear_error (&error);
    return 1;
  }
  int page_count = fw_document_get_page_count (doc);
  fprintf (stderr, "self-test: %d pages\n", page_count);

  FwCache *cache = fw_cache_new (doc, NULL);
  fw_cache_set_scale_factor (cache, 1);
  fw_cache_start (cache, 1.0, 0);
  int p0[1] = { 0 };
  fw_cache_set_priority (cache, p0, 1);

  /* Wait for page 0 to actually render. Mirror what
   * bench-startup does — page-0 paint is the load-bearing event;
   * if the binary boots but never paints, that's a regression. */
  GMainContext *ctx = g_main_context_default ();
  gint64 deadline = g_get_monotonic_time () + (gint64) SELF_TEST_TIMEOUT_MS * 1000;
  gboolean painted = FALSE;
  while (g_get_monotonic_time () < deadline) {
    g_main_context_iteration (ctx, FALSE);
    if (fw_cache_get_texture (cache, 0)) { painted = TRUE; break; }
    g_usleep (1000);
  }
  if (!painted) {
    fprintf (stderr, "self-test: page 0 did not paint within %d ms\n",
             SELF_TEST_TIMEOUT_MS);
    fw_cache_stop (cache);
    g_object_unref (cache);
    g_object_unref (doc);
    return 1;
  }

  /* Light scrub — surfaces a class of bugs (worker queue stalls,
   * deadlocks under cancel_gen churn) that wouldn't surface from a
   * single render. Walks every Nth page across the doc, briefly. */
  int stride = page_count / 12;
  if (stride < 1) stride = 1;
  for (int pg = 0; pg < page_count; pg += stride) {
    int single[1] = { pg };
    fw_cache_set_priority (cache, single, 1);
    gint64 t = g_get_monotonic_time () + 30 * 1000;
    while (g_get_monotonic_time () < t)
      g_main_context_iteration (ctx, FALSE);
  }

  fw_cache_stop (cache);
  while (g_main_context_iteration (ctx, FALSE)) { }
  g_object_unref (cache);
  g_object_unref (doc);

  fprintf (stderr, "self-test: PASS\n");
  return 0;
}
#endif  /* FW_STRESS_BUILD */

int
main (int argc, char *argv[])
{
  for (int i = 1; i < argc; i++) {
    if (strcmp (argv[i], "--version") == 0 ||
        strcmp (argv[i], "-v") == 0) {
      printf ("Framework %s\n", APP_VERSION);
      return 0;
    }
    if (strcmp (argv[i], "--self-test") == 0) {
#if FW_STRESS_BUILD
      const char *path = (i + 1 < argc) ? argv[i + 1] : SELF_TEST_DEFAULT;
      return run_self_test (path);
#else
      fprintf (stderr,
               "framework: --self-test is only available in builds configured "
               "with -Dstress=true.\n");
      return 2;
#endif
    }
  }

  fw_debug_init ();

  /* Apply Landlock LSM hardening before launching the app loop.
   * Drops EXECUTE + MAKE_* filesystem rights so a malicious document
   * exploiting a backend (MuPDF, DjVuLibre, libarchive) can't
   * escalate to spawning a shell or planting a backdoor file.
   * Read/write_file stay allowed so state.json, save-attachments,
   * and print spool writes continue to work. No-op on non-Linux
   * and on kernels without Landlock support. */
  fw_sandbox_drop_execute ();

  g_autoptr (FwApplication) app = fw_application_new ();
  return g_application_run (G_APPLICATION (app), argc, argv);
}
