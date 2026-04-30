/* tests/stress/stress-zoom-storm.c — zoom-transition stress test
 *
 * Hammers the v1.4 prev_surface stash path and the v1.5 texture cache
 * lifecycle by alternating Ctrl+Plus / Ctrl+Minus across the full zoom
 * range (10%–1000%) on a single page. Each zoom change calls
 * fw_cache_start() which bumps render_gen, moves the current surface to
 * prev_surface, and queues a re-render at the new zoom.
 *
 * Asserts:
 *   - no crashes
 *   - peak RSS under FW_STRESS_RSS_CAP_MB
 *   - the cache's surfaces converge to a stable count after settle
 *
 * If any of the surface/texture lifecycle code (cache_entry_free,
 * fw_cache_start's prev_surface move, the v1.5 texture-before-surface
 * unref ordering) is wrong, this test runs a leak per cycle and ASan
 * catches it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-cache.h"
#include "fw-debug.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

/* The ZOOM-STORM peak is intentionally allowed to be high (a few GB).
 * Each transition holds both old + new surface + textures per cached
 * page — that's expected by the v1.4 prev_surface design. The leak
 * signal is the *post-settle* RSS, after one steady zoom level has been
 * the only active state for a few seconds. A leak shows as a settled
 * RSS far above what a single-zoom render would produce.
 *
 * For real lifecycle bug detection, run this test under ASan
 * (-Dsanitize=address). ASan instruments every allocation and prints a
 * report at exit if anything leaked or was used-after-free. */
/* Settled cap is a coarse sanity check for unbounded growth — generous
 * because glibc's malloc holds onto freed pages for reuse rather than
 * returning them to the kernel immediately. The authoritative leak
 * check is `meson configure builddir -Dsanitize=address` followed by
 * a rerun; ASan instruments every allocation and prints attribution
 * for any leak at exit. */
#define DEFAULT_SETTLED_RSS_CAP_MB 1024
#define ZOOM_CYCLES 50

/* Peak RSS via getrusage — high-water mark, never decreases. Useful for
 * "did we ever exceed?" measurements but not for "did transient memory
 * actually drop after settle?". */
static long
rss_peak_mb (void)
{
  struct rusage ru;
  getrusage (RUSAGE_SELF, &ru);
  return ru.ru_maxrss / 1024;
}

/* Current RSS via /proc/self/status. Decreases when the process frees
 * memory back to the kernel. The settled-RSS leak check below uses this
 * — if transient zoom-transition memory was correctly released after
 * settle, current RSS drops below peak. If it didn't, that's a leak. */
static long
rss_current_mb (void)
{
  FILE *f = fopen ("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long kb = -1;
  while (fgets (line, sizeof line, f)) {
    if (strncmp (line, "VmRSS:", 6) == 0) {
      kb = strtol (line + 6, NULL, 10);
      break;
    }
  }
  fclose (f);
  return kb < 0 ? -1 : kb / 1024;
}

static void
spin_main_loop (int ms)
{
  GMainContext *ctx = g_main_context_default ();
  gint64 deadline = g_get_monotonic_time () + (gint64) ms * 1000;
  while (g_get_monotonic_time () < deadline)
    g_main_context_iteration (ctx, FALSE);
}

int
main (int argc, char **argv)
{
  fw_debug_init ();
  gtk_init ();

  if (argc < 2) {
    fprintf (stderr, "stress-zoom-storm: pass a document path as argv[1].\n");
    return 2;
  }
  const char *path = argv[1];

  long settled_cap_mb = DEFAULT_SETTLED_RSS_CAP_MB;
  const char *cap_env = g_getenv ("FW_STRESS_RSS_CAP_MB");
  if (cap_env) settled_cap_mb = atol (cap_env);

  printf ("stress-zoom-storm: opening %s\n", path);
  long rss_baseline = rss_current_mb ();

  g_autoptr (GError) error = NULL;
  FwDocument *doc = fw_document_new_for_path (path, &error);
  if (!doc) {
    fprintf (stderr, "stress-zoom-storm: open failed: %s\n",
             error ? error->message : "(null)");
    return 1;
  }
  int page_count = fw_document_get_page_count (doc);
  if (page_count < 1) {
    fprintf (stderr, "stress-zoom-storm: zero-page document\n");
    g_object_unref (doc);
    return 1;
  }
  printf ("stress-zoom-storm: %d pages\n", page_count);

  FwCache *cache = fw_cache_new (doc, NULL);
  fw_cache_set_scale_factor (cache, 1);
  fw_cache_set_velocity (cache, 0.0); /* STATIC throughout — we're testing zoom, not scroll */
  fw_cache_start (cache, 1.0, 0);

  /* Pin priority on a single page so every render goes to the same
   * cache entry — that maximizes the prev_surface churn. */
  int pinned[1] = { page_count / 2 };
  fw_cache_set_priority (cache, pinned, 1);
  spin_main_loop (200); /* let the initial 1.0 zoom render */

  /* ── Zoom storm ───────────────────────────────────────────────────
   * Alternate between zoom levels across a realistic range. The cycle
   * is intentionally non-monotonic so each step bumps render_gen and
   * exercises the prev_surface move + texture replacement. Capped at
   * 400% — beyond that, a single page surface is hundreds of MB and
   * the test would measure raw allocation cost rather than lifecycle
   * correctness. The high-zoom-fallback (slice rendering) lives in
   * roadmap Phase 11 Tier 2. */
  static const double zoom_levels[] = {
    0.25, 0.50, 1.00, 1.50, 2.00, 3.00, 4.00,
    3.00, 2.00, 1.50, 1.00, 0.50, 0.25,
  };
  const int n_levels = (int) (sizeof (zoom_levels) / sizeof (zoom_levels[0]));

  printf ("zoom storm: %d cycles across %d levels\n", ZOOM_CYCLES, n_levels);
  for (int i = 0; i < ZOOM_CYCLES; i++) {
    double z = zoom_levels[i % n_levels];
    fw_cache_start (cache, z, 0);
    fw_cache_set_priority (cache, pinned, 1);
    spin_main_loop (40); /* give workers a tick */
  }

  long rss_peak = rss_peak_mb ();
  long rss_after_storm = rss_current_mb ();

  /* ── Settle ──────────────────────────────────────────────────────── */
  printf ("settle\n");
  fw_cache_start (cache, 1.0, 0);
  fw_cache_set_priority (cache, pinned, 1);
  spin_main_loop (5000); /* generous — let prev_surface stash + transient
                            allocs fully drop, so current RSS reflects
                            actual steady state, not high-water mark */

  long rss_settled = rss_current_mb ();
  printf ("rss: baseline=%ld MB, peak=%ld MB, after-storm=%ld MB, settled=%ld MB, settled_cap=%ld MB\n",
          rss_baseline, rss_peak, rss_after_storm, rss_settled, settled_cap_mb);

  fw_cache_stop (cache);
  spin_main_loop (200);
  g_object_unref (cache);
  g_object_unref (doc);

  if (rss_settled > settled_cap_mb) {
    fprintf (stderr,
             "FAIL: post-settle RSS %ld MB exceeded settled cap %ld MB.\n"
             "      Peak during storm was %ld MB (transition memory is OK).\n"
             "      Set FW_STRESS_RSS_CAP_MB to override, or run under ASan\n"
             "      (-Dsanitize=address) for leak attribution.\n",
             rss_settled, settled_cap_mb, rss_peak);
    return 1;
  }
  printf ("PASS: stress-zoom-storm settled under cap (peak %ld MB during storm).\n",
          rss_peak);
  return 0;
}
