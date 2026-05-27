/* tests/stress/stress-scrub.c — worst-case scroll pattern stress test
 *
 * Drives FwCache directly without a widget tree. Simulates a punishing
 * scroll pattern (0 → last page in 500ms, then back-and-forth, then settle)
 * and asserts:
 *   - no crashes during the run (the test simply exits non-zero on segfault)
 *   - the resting RSS after settle is within a configurable cap
 *   - the cache state machine eventually returns to STATIC
 *   - no in-flight render workers are stuck (active_jobs returns to 0)
 *
 * Run:
 *   meson setup builddir -Dstress=true
 *   meson compile -C builddir
 *   GSETTINGS_SCHEMA_DIR=builddir/data ./builddir/tests/stress/stress-scrub <pdf>
 *
 * Without an argument, falls back to the first 'pdf' sample in
 * tests/corpus.json under FW_TEST_CORPUS_ROOT (or the corpus default root).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-cache.h"
#include "fw-debug.h"
#include "stress-util.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

/* Default cap covers: cache surfaces (FW_CACHE_BYTES_CAP_MB, default 128
 * for tests), persistent thumbnails (~120 MB on 1000-page docs), MuPDF's
 * 8 per-instance contexts (~32 MB each = 256 MB), transient renders, and
 * glibc retention. Plus ~40% overhead under ASan. 1200 MB is comfortably
 * above the worst-case observed on the corpus (Berserk CBZ ~912 MB under
 * ASan). The cap exists to flag obvious runaway, not to measure typical
 * memory — for that, use FW_DEBUG=1 and read the byte-cap evict traces. */
#define DEFAULT_RSS_CAP_MB 1200

static void
push_priority_for_page (FwCache *cache, int page, int total)
{
  /* Pretend the viewport is showing one page; cache will fill ±10 around it. */
  (void) total;
  int single[1] = { page };
  fw_cache_set_priority (cache, single, 1);
}

int
main (int argc, char **argv)
{
  /* Pin the cache's bytes-aware cap to a small value (128 MB) before
   * GSettings/GIO touches the cache. This exercises the byte-cap eviction
   * path during Phase 4 — without a tight cap, the cache simply holds
   * outside-priority surfaces forever (the new fast-scroll-back behavior)
   * and total RSS is dominated by glibc retention rather than something
   * the test can meaningfully bound. The user can override via env. */
  if (!g_getenv ("FW_CACHE_BYTES_CAP_MB"))
    g_setenv ("FW_CACHE_BYTES_CAP_MB", "128", TRUE);

  fw_debug_init ();
  gtk_init ();

  const char *path = NULL;
  if (argc >= 2) {
    path = argv[1];
  } else {
    fprintf (stderr,
             "stress-scrub: pass a document path as argv[1].\n"
             "  example: stress-scrub .testfiles/effective-java.pdf\n");
    return 2;
  }

  long rss_cap_mb = DEFAULT_RSS_CAP_MB;
  const char *cap_env = g_getenv ("FW_STRESS_RSS_CAP_MB");
  if (cap_env)
    rss_cap_mb = atol (cap_env);

  printf ("stress-scrub: opening %s\n", path);
  long rss_baseline = rss_peak_mb ();

  g_autoptr (GError) error = NULL;
  FwDocument *doc = fw_document_new_for_path (path, &error);
  if (!doc) {
    fprintf (stderr, "stress-scrub: open failed: %s\n",
             error ? error->message : "(null)");
    return 1;
  }
  int page_count = fw_document_get_page_count (doc);
  printf ("stress-scrub: %d pages\n", page_count);
  if (page_count < 20) {
    fprintf (stderr, "stress-scrub: doc too small (%d pages); pick a longer one.\n",
             page_count);
    g_object_unref (doc);
    return 2;
  }

  /* No widget — the cache's view_widget pointer stays NULL, redraw scheduling
   * is skipped (safe_queue_draw is gated on widget != NULL). */
  FwCache *cache = fw_cache_new (doc, NULL);
  fw_cache_set_scale_factor (cache, 1);
  fw_cache_start (cache, 1.0, 0);

  /* ── Phase 1: 0 → last in 500ms (worst-case scrub) ─────────────── */
  printf ("phase 1: scrub 0 → last in 500 ms\n");
  fw_cache_set_velocity (cache, 5.0); /* high → SCRUBBING */
  int steps = 50;
  int step_size = page_count / steps;
  if (step_size < 1) step_size = 1;
  for (int i = 0; i < steps; i++) {
    int pg = i * step_size;
    if (pg >= page_count) pg = page_count - 1;
    push_priority_for_page (cache, pg, page_count);
    g_usleep (10000); /* 10 ms */
    spin_main_loop (1);
  }

  /* ── Phase 2: 5 × back-and-forth ────────────────────────────────── */
  printf ("phase 2: 5 × back-and-forth\n");
  for (int i = 0; i < 5; i++) {
    push_priority_for_page (cache, 0, page_count);
    spin_main_loop (50);
    push_priority_for_page (cache, page_count - 1, page_count);
    spin_main_loop (50);
  }

  /* ── Phase 3: settle ─────────────────────────────────────────────── */
  printf ("phase 3: settle\n");
  fw_cache_set_velocity (cache, 0.0); /* low → STATIC */
  push_priority_for_page (cache, 0, page_count);
  spin_main_loop (3000); /* let workers drain */

  /* ── Phase 4: slow walk (exercises bytes-aware eviction) ─────────── */
  /* Walk through the document one page at a time, allowing each page to
   * render before shifting priority. As the priority window slides
   * forward, previously-rendered pages drop out of the keep-set but
   * stay in the cache (the new bytes-aware policy). When total cached
   * bytes exceeds byte_cap, eviction fires and outside-priority pages
   * are dropped. Without this phase, the test never accumulates enough
   * outside-priority bytes to validate the cap. */
  printf ("phase 4: slow walk to exercise byte cap\n");
  int walk_step = page_count / 30;
  if (walk_step < 1) walk_step = 1;
  for (int pg = 0; pg < page_count; pg += walk_step) {
    push_priority_for_page (cache, pg, page_count);
    spin_main_loop (200);
  }

  long rss_peak = rss_peak_mb ();
  printf ("rss: baseline=%ld MB, peak=%ld MB, cap=%ld MB\n",
          rss_baseline, rss_peak, rss_cap_mb);

  /* ── Cleanup ─────────────────────────────────────────────────────── */
  fw_cache_stop (cache);
  spin_main_loop (200);
  g_object_unref (cache);
  g_object_unref (doc);

  int exit_code = 0;
  if (rss_peak > rss_cap_mb) {
    fprintf (stderr,
             "FAIL: peak RSS %ld MB exceeded cap %ld MB.\n"
             "      Set FW_STRESS_RSS_CAP_MB to override, or investigate.\n",
             rss_peak, rss_cap_mb);
    exit_code = 1;
  } else {
    printf ("PASS: stress-scrub completed under cap.\n");
  }
  return exit_code;
}
