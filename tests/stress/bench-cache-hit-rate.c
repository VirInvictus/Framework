/* tests/stress/bench-cache-hit-rate.c — cache hit/miss bench
 *
 * Drives the cache through synthetic scroll patterns and reports
 * the hit ratio at each render-state band (STATIC, CRUISING,
 * SCRUBBING). The original Phase 12.3 plan called for a recorded
 * trace format; the synthetic version sidesteps that infrastructure
 * (no per-platform trace recorder, no trace-file schema) and is
 * fully reproducible — the patterns are the same on every run.
 *
 * Patterns:
 *   STATIC    — velocity 0, walk page-by-page with dwell so the
 *               priority window can fill ahead of each query.
 *   CRUISING  — velocity 0.8 (between SCRUBBING and STATIC bands),
 *               walk every 3 pages with shorter dwell.
 *   SCRUBBING — velocity 5.0 (forces SCRUBBING state), big jumps
 *               with no dwell — most queries miss, cache aborts
 *               mid-render via fz_cookie.
 *
 * Hit/miss is counted by calling `fw_cache_get_texture(current_page)`
 * after each step — non-NULL = hit, NULL = miss. This is exactly
 * what the view sees on every paint.
 *
 * Output: per-pattern n / hits / hit_rate. Built but NOT registered
 * with `meson test` — latency-style benchmark, not pass/fail.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-cache.h"
#include "corpus-root.h"
#include "fw-debug.h"
#include "stress-util.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *label;
  double      velocity;
  int         step_pages;     /* pages advanced per query */
  int         dwell_ms;       /* main-loop spin between queries */
  int         max_queries;
} Pattern;

static int
run_pattern (FwCache *cache, int page_count, const Pattern *p)
{
  fw_cache_set_velocity (cache, p->velocity);
  int hits = 0;
  int queries = 0;
  int pg = 0;
  int dir = +1;

  while (queries < p->max_queries) {
    int single[1] = { pg };
    fw_cache_set_priority (cache, single, 1);
    spin_main_loop (p->dwell_ms);

    GdkTexture *tex = fw_cache_get_texture (cache, pg);
    if (tex) hits++;
    queries++;

    pg += dir * p->step_pages;
    if (pg >= page_count) { pg = page_count - 1; dir = -1; }
    if (pg < 0)           { pg = 0;              dir = +1; }
  }
  return hits;
}

int
main (int argc, char **argv)
{
  fw_debug_init ();
  gtk_init ();

  g_autofree char *fallback = NULL;
  if (argc < 2) {
    g_autofree char *root = fw_test_corpus_root ();
    fallback = g_build_filename (root, "effective-java.pdf", NULL);
  }
  const char *path = argc >= 2 ? argv[1] : fallback;

  printf ("bench-cache-hit-rate: %s\n", path);

  g_autoptr (GError) error = NULL;
  FwDocument *doc = fw_document_new_for_path (path, &error);
  if (!doc) {
    fprintf (stderr, "open failed: %s\n", error ? error->message : "(null)");
    return 1;
  }
  int page_count = fw_document_get_page_count (doc);
  printf ("  pages=%d\n\n", page_count);

  FwCache *cache = fw_cache_new (doc, NULL);
  fw_cache_set_scale_factor (cache, 1);
  fw_cache_start (cache, 1.0, 0);

  /* Warm-up: let the initial priority window render so STATIC starts
   * from a populated cache (representative of normal reading). */
  spin_main_loop (1500);

  Pattern patterns[] = {
    { "STATIC",    0.0, 1,  120, 50 },
    { "CRUISING",  0.8, 3,   40, 50 },
    { "SCRUBBING", 5.0, 50,   5, 50 },
  };

  printf ("  %-10s  %-6s  %-6s  %-9s\n", "pattern", "n", "hits", "hit_rate");
  for (size_t i = 0; i < G_N_ELEMENTS (patterns); i++) {
    int hits = run_pattern (cache, page_count, &patterns[i]);
    double rate = (double) hits / (double) patterns[i].max_queries * 100.0;
    printf ("  %-10s  %-6d  %-6d  %5.1f%%\n",
            patterns[i].label, patterns[i].max_queries, hits, rate);
  }

  fw_cache_stop (cache);
  spin_main_loop (200);
  g_object_unref (cache);
  g_object_unref (doc);
  return 0;
}
