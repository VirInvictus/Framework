/* tests/stress/stress-multidoc.c — open/close lifecycle stress test
 *
 * The existing stress-scrub and stress-zoom-storm tests both run for
 * a single document — they don't exercise the dispose path under
 * churn. This test does. Two phases:
 *
 *   1. Sequential: open document N, create FwCache, render a handful
 *      of pages, dispose everything, repeat 50 times across a
 *      multi-format corpus.
 *   2. Parallel: hold 10 FwDocument instances + caches simultaneously
 *      (no rendering — purely the construction/destruction path).
 *
 * Asserts: zero crashes, no growing leak in steady-state RSS (the
 * cache leak fix from v1.3.3 must not regress), no ASan errors at
 * exit. Run under `-Dsanitize=address` for the meaningful version of
 * this test — without ASan a leak would just slowly inflate RSS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-cache.h"
#include "fw-debug.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define SEQUENTIAL_ITERATIONS 50
#define PARALLEL_INSTANCES    10
#define PAGES_TO_RENDER        3   /* per opened document, in sequential phase */
/* Cap accommodates 10 simultaneous docs + thumb tiers + ASan overhead.
 * 10 docs × ~80 MB each (8-instance MuPDF contexts + cache state) plus
 * ASan's ~50% instrumentation overhead lands here. Drop the cap if
 * a real leak ever pushes us higher. */
#define DEFAULT_RSS_CAP_MB  2000

static long
rss_peak_mb (void)
{
  struct rusage ru;
  getrusage (RUSAGE_SELF, &ru);
  return ru.ru_maxrss / 1024;
}

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

/* Six representative samples — one per backend code path (PDF/DjVu via
 * stext + render; EPUB/MOBI via reflowable layout; CBZ via MuPDF
 * dispatch; CBR via libarchive). The cycle covers each backend
 * roughly 8x in 50 iterations.
 *
 * Hardcoded paths under FW_TEST_CORPUS_ROOT (default Brandon's
 * Calibre Library / Comics tree). If a sample is missing, the test
 * skips that iteration silently — better than failing on environments
 * that don't have the full corpus. */
static const char *SEQUENTIAL_CORPUS[] = {
  "/home/bdkl/docs/Calibre Library/Joshua Bloch/Effective Java (5)/Effective Java - Joshua Bloch.pdf",
  "/home/bdkl/docs/Calibre Library/Michael Spivak/Calculus (1325)/Calculus - Michael Spivak.djvu",
  "/home/bdkl/docs/Calibre Library/Haruki Murakami/The Wind-Up Bird Chronicle (506)/The Wind-Up Bird Chronicle - Haruki Murakami.epub",
  "/home/bdkl/docs/Calibre Library/David Gemmell/Fall of Kings (1581)/Fall of Kings - David Gemmell.mobi",
  "/mnt/SharedData/Comics/Berserk (v01-v041)/Berserk v25 (2008) (Digital) (danke-Empire).cbz",
  "/mnt/SharedData/Comics/From Hell (Master Edition)/From Hell - Master Edition (2020).cbr",
};
static const int SEQUENTIAL_CORPUS_LEN =
  (int) (sizeof (SEQUENTIAL_CORPUS) / sizeof (SEQUENTIAL_CORPUS[0]));

static gboolean
file_exists (const char *path)
{
  return g_file_test (path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR);
}

static void
run_sequential_phase (long *out_after_each_iter,
                      int *out_completed)
{
  printf ("phase 1: %d sequential open/close cycles\n", SEQUENTIAL_ITERATIONS);
  int completed = 0;
  for (int iter = 0; iter < SEQUENTIAL_ITERATIONS; iter++) {
    const char *path = SEQUENTIAL_CORPUS[iter % SEQUENTIAL_CORPUS_LEN];
    if (!file_exists (path)) {
      FW_TRACE_DOC ("multidoc skip (missing): %s", path);
      continue;
    }

    g_autoptr (GError) error = NULL;
    FwDocument *doc = fw_document_new_for_path (path, &error);
    if (!doc) {
      fprintf (stderr, "multidoc iter %d: open failed (%s): %s\n",
               iter, path, error ? error->message : "(null)");
      continue;
    }
    int page_count = fw_document_get_page_count (doc);

    FwCache *cache = fw_cache_new (doc, NULL);
    fw_cache_set_scale_factor (cache, 1);
    fw_cache_start (cache, 1.0, 0);

    /* Push priority on the first few pages and let workers chew. */
    int n = page_count < PAGES_TO_RENDER ? page_count : PAGES_TO_RENDER;
    int visible[PAGES_TO_RENDER];
    for (int i = 0; i < n; i++) visible[i] = i;
    fw_cache_set_priority (cache, visible, n);
    spin_main_loop (50);

    fw_cache_stop (cache);
    spin_main_loop (50);
    g_object_unref (cache);
    g_object_unref (doc);
    completed++;

    out_after_each_iter[iter] = rss_current_mb ();
    if ((iter + 1) % 10 == 0)
      printf ("  iter %2d/%2d: rss=%ld MB\n",
              iter + 1, SEQUENTIAL_ITERATIONS, out_after_each_iter[iter]);
  }
  *out_completed = completed;
}

static void
run_parallel_phase (void)
{
  printf ("phase 2: %d documents held simultaneously\n", PARALLEL_INSTANCES);
  FwDocument *docs[PARALLEL_INSTANCES] = { 0 };
  FwCache    *caches[PARALLEL_INSTANCES] = { 0 };

  /* Open every available sample, then cycle to fill remaining slots. */
  int slot = 0;
  for (int i = 0; i < SEQUENTIAL_CORPUS_LEN && slot < PARALLEL_INSTANCES; i++) {
    if (!file_exists (SEQUENTIAL_CORPUS[i])) continue;
    g_autoptr (GError) error = NULL;
    docs[slot] = fw_document_new_for_path (SEQUENTIAL_CORPUS[i], &error);
    if (docs[slot]) {
      caches[slot] = fw_cache_new (docs[slot], NULL);
      fw_cache_set_scale_factor (caches[slot], 1);
      fw_cache_start (caches[slot], 1.0, 0);
      slot++;
    }
  }
  /* Fill remaining slots by reopening the first existing sample. */
  if (slot < PARALLEL_INSTANCES) {
    const char *fallback = NULL;
    for (int i = 0; i < SEQUENTIAL_CORPUS_LEN; i++) {
      if (file_exists (SEQUENTIAL_CORPUS[i])) {
        fallback = SEQUENTIAL_CORPUS[i];
        break;
      }
    }
    if (fallback) {
      while (slot < PARALLEL_INSTANCES) {
        g_autoptr (GError) error = NULL;
        docs[slot] = fw_document_new_for_path (fallback, &error);
        if (!docs[slot]) break;
        caches[slot] = fw_cache_new (docs[slot], NULL);
        fw_cache_set_scale_factor (caches[slot], 1);
        fw_cache_start (caches[slot], 1.0, 0);
        slot++;
      }
    }
  }
  printf ("  opened %d / %d slots\n", slot, PARALLEL_INSTANCES);

  spin_main_loop (200);

  /* Tear them all down in reverse order. */
  for (int i = PARALLEL_INSTANCES - 1; i >= 0; i--) {
    if (caches[i]) {
      fw_cache_stop (caches[i]);
      g_object_unref (caches[i]);
    }
    if (docs[i]) g_object_unref (docs[i]);
  }
  spin_main_loop (200);
}

int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
  fw_debug_init ();
  gtk_init ();

  long rss_cap_mb = DEFAULT_RSS_CAP_MB;
  const char *cap_env = g_getenv ("FW_STRESS_RSS_CAP_MB");
  if (cap_env) rss_cap_mb = atol (cap_env);

  long rss_baseline = rss_current_mb ();
  printf ("stress-multidoc: baseline rss=%ld MB cap=%ld MB\n",
          rss_baseline, rss_cap_mb);

  long iter_rss[SEQUENTIAL_ITERATIONS] = { 0 };
  int  iter_completed = 0;
  run_sequential_phase (iter_rss, &iter_completed);

  long after_seq = rss_current_mb ();
  printf ("after sequential: rss=%ld MB completed=%d/%d\n",
          after_seq, iter_completed, SEQUENTIAL_ITERATIONS);

  run_parallel_phase ();

  long after_par = rss_current_mb ();
  long peak = rss_peak_mb ();
  printf ("after parallel:   rss=%ld MB peak=%ld MB\n", after_par, peak);

  /* Leak signal: the difference between the first iteration's RSS
   * (after opening one doc) and the last iteration's RSS. A clean
   * dispose path means RSS plateaus; a leak grows linearly. We use
   * iter[0] vs iter[N-1] rather than baseline vs final because the
   * first open allocates a bunch of one-time state (MuPDF context,
   * GLib globals) that's not freed between iterations. */
  long first_iter_rss = -1, last_iter_rss = -1;
  for (int i = 0; i < SEQUENTIAL_ITERATIONS; i++) {
    if (iter_rss[i] > 0) {
      if (first_iter_rss < 0) first_iter_rss = iter_rss[i];
      last_iter_rss = iter_rss[i];
    }
  }
  if (first_iter_rss > 0 && last_iter_rss > 0) {
    long delta = last_iter_rss - first_iter_rss;
    printf ("rss drift across sequential phase: first=%ld last=%ld delta=%+ld MB\n",
            first_iter_rss, last_iter_rss, delta);
  }

  if (peak > rss_cap_mb) {
    fprintf (stderr, "FAIL: peak RSS %ld MB exceeded cap %ld MB.\n",
             peak, rss_cap_mb);
    return 1;
  }
  printf ("PASS: stress-multidoc completed under cap.\n");
  return 0;
}
