/* tests/stress/bench-startup.c — open-to-first-paint latency
 *
 * Times each corpus sample's full warm-up flow:
 *
 *   1. fw_document_new_for_path             →  open_ms
 *   2. fw_cache_start + priority [0] + wait → first_paint_ms
 *      (spin the main loop until fw_cache_get_texture(0) returns
 *       non-NULL — this is the actual user-visible "page 0 painted"
 *       moment, not just "the worker queue is empty")
 *
 * Reports per-file timings as a table. No pass/fail — this is a
 * benchmark, not a stress test, so it is built but NOT registered
 * with `meson test`. Catches regressions in `apply_fit_width_tick`,
 * the priority-window initial submit, or backend `open` paths.
 *
 * Usage:
 *   bench-startup                       # run the canonical corpus
 *   bench-startup <doc> [<doc> ...]     # run only the listed docs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-cache.h"
#include "fw-debug.h"
#include "corpus-root.h"
#include "stress-util.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#define FIRST_PAINT_TIMEOUT_MS 30000   /* very generous; comics on RAR can be slow */

/* Default corpus as filenames; resolved against the corpus root at
 * runtime (see corpus-root.h). Pass paths as argv to override. */
static const char *DEFAULT_CORPUS_FILES[] = {
  "effective-java.pdf",
  "visual-explanations-tufte.pdf",
  "on-growth-and-form.djvu",
  "playing-at-the-world-v2.epub",
  "the-broken-god.mobi",
  "datapoint.azw3",
  "nausicaa-v01.cbz",
  "vagabond-v01.cbr",
};
static const int DEFAULT_CORPUS_LEN =
  (int) (sizeof (DEFAULT_CORPUS_FILES) / sizeof (DEFAULT_CORPUS_FILES[0]));

static const char *
display_name (const char *path)
{
  const char *base = strrchr (path, '/');
  return base ? base + 1 : path;
}

/* Iterate the main loop until either the page-0 texture is ready or
 * the deadline expires. Returns the elapsed µs from `t_start` to the
 * moment the texture first appeared, or -1 on timeout. */
static gint64
wait_for_first_paint (FwCache *cache, int page, gint64 t_start, int timeout_ms)
{
  GMainContext *ctx = g_main_context_default ();
  gint64 deadline = t_start + (gint64) timeout_ms * 1000;
  while (g_get_monotonic_time () < deadline) {
    g_main_context_iteration (ctx, FALSE);
    GdkTexture *tex = fw_cache_get_texture (cache, page);
    if (tex)
      return g_get_monotonic_time () - t_start;
    g_usleep (1000);  /* 1 ms — light cooperation, doesn't move the needle */
  }
  return -1;
}

static gboolean
bench_one (const char *path)
{
  if (!file_exists (path)) {
    printf ("  %-46s  (missing)\n", display_name (path));
    return TRUE;
  }

  gint64 t0 = g_get_monotonic_time ();

  g_autoptr (GError) error = NULL;
  FwDocument *doc = fw_document_new_for_path (path, &error);
  if (!doc) {
    fprintf (stderr, "  %-46s  open failed: %s\n",
             display_name (path), error ? error->message : "(null)");
    return FALSE;
  }
  gint64 t1 = g_get_monotonic_time ();

  FwCache *cache = fw_cache_new (doc, NULL);
  fw_cache_set_scale_factor (cache, 1);
  fw_cache_start (cache, 1.0, 0);
  int single[1] = { 0 };
  fw_cache_set_priority (cache, single, 1);

  gint64 paint_us = wait_for_first_paint (cache, 0, t1, FIRST_PAINT_TIMEOUT_MS);

  fw_cache_stop (cache);
  /* Drain pending idle callbacks before unref so safe_queue_draw
   * doesn't fire on a freed widget pointer. */
  GMainContext *ctx = g_main_context_default ();
  while (g_main_context_iteration (ctx, FALSE)) { }
  g_object_unref (cache);
  g_object_unref (doc);

  double open_ms  = (double) (t1 - t0) / 1000.0;
  double paint_ms = paint_us > 0 ? (double) paint_us / 1000.0 : -1.0;
  double total_ms = paint_us > 0 ? open_ms + paint_ms : -1.0;

  if (paint_us < 0)
    printf ("  %-46s  open=%7.1f ms  paint=  TIMEOUT  total=  TIMEOUT\n",
            display_name (path), open_ms);
  else
    printf ("  %-46s  open=%7.1f ms  paint=%7.1f ms  total=%7.1f ms\n",
            display_name (path), open_ms, paint_ms, total_ms);
  return paint_us > 0;
}

int
main (int argc, char **argv)
{
  fw_debug_init ();
  gtk_init ();

  printf ("bench-startup: open-to-first-paint per sample\n");
  printf ("  %-46s  %-13s  %-14s  %s\n",
          "sample", "open", "first paint", "total");

  const char **corpus;
  int corpus_len;
  g_auto (GStrv) resolved = NULL;
  if (argc >= 2) {
    corpus     = (const char **) (argv + 1);
    corpus_len = argc - 1;
  } else {
    g_autofree char *root = fw_test_corpus_root ();
    resolved = g_new0 (char *, DEFAULT_CORPUS_LEN + 1);
    for (int i = 0; i < DEFAULT_CORPUS_LEN; i++)
      resolved[i] = g_build_filename (root, DEFAULT_CORPUS_FILES[i], NULL);
    corpus     = (const char **) resolved;
    corpus_len = DEFAULT_CORPUS_LEN;
  }

  int failures = 0;
  for (int i = 0; i < corpus_len; i++) {
    if (!bench_one (corpus[i]))
      failures++;
  }
  return failures > 0 ? 1 : 0;
}
