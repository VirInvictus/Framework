/* regress-webview-anchor.c — Regression net for the position-reporter
 * anchor guard (2026-09-16).
 *
 * The reflow position reporter picks the first [id] element at or below
 * the viewport top as the restore anchor. Without a rendered-box guard,
 * our own injected reading stylesheet — <style id=fw-reading-css> in
 * head, non-rendered, rect.top always 0 — always won the pick, so every
 * saved position carried that anchor and every restore scrolled back to
 * the top of the document instead of the saved scroll_y. The user script
 * now also requires r.height > 0.
 *
 * The JS only executes inside a WebKitWebView, which the headless suite
 * never instantiates (tests/README.md: presentation is verified by
 * running the app). So the guard is pinned by source inspection: the
 * guarded form must be present and the unguarded regression shape must
 * be gone. G_TEST_SRCDIR resolves the source tree; a bare run falls back
 * to ./src relative to the working directory.
 */

#include "corpus-root.h"

#include <glib.h>

static char *
source_path (const char *relative)
{
  const char *srcdir = g_getenv ("G_TEST_SRCDIR");
  if (srcdir && srcdir[0])
    return g_build_filename (srcdir, relative, NULL);
  return g_strdup (relative);
}

static void
test_position_reporter_requires_rendered_box (void)
{
  g_autofree char *path = source_path ("src/fw-webview.c");
  g_autofree char *text = NULL;

  if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      g_test_skip ("src/fw-webview.c not found (run via meson test)");
      return;
    }
  if (!g_file_get_contents (path, &text, NULL, NULL))
    {
      g_test_fail ();
      g_test_message ("could not read src/fw-webview.c");
      return;
    }

  g_test_message ("the anchor pick must require a rendered box "
                  "(r.top >= 0 && r.height > 0)");
  if (!strstr (text, "r.top >= 0 && r.height > 0"))
    {
      g_test_fail ();
      g_test_message ("rendered-box guard missing from the position "
                      "reporter; restores would land at the top again");
    }

  g_test_message ("the unguarded pick must stay gone");
  if (strstr (text, "if (r.top >= 0) { a = nodes[i].id; break; }"))
    {
      g_test_fail ();
      g_test_message ("unguarded anchor pick reintroduced");
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/regress/webview/position-reporter-rendered-box",
                   test_position_reporter_requires_rendered_box);
  return g_test_run ();
}
