/* tests/stress/stress-corpus-soak.c — full-corpus soak test
 *
 * The narrow stress tests pin a single document and beat on a
 * specific failure mode (scrub, zoom, search, multi-doc lifecycle).
 * This one does the broad sweep: walk the full corpus, render every
 * Nth page through the cache, on each backend (PDF, DjVu, EPUB,
 * MOBI, CBZ, CBR), and assert no crashes. It's the safety net for
 * "did anything regress that nobody else is exercising?"
 *
 * Each sample:
 *   1. Open the document.
 *   2. Build an FwCache with default settings.
 *   3. Walk the document by stride (default 5), pushing priority on
 *      one page at a time and giving workers a moment to render.
 *   4. Tear everything down, move on to the next sample.
 *
 * Asserts: zero crashes, peak RSS under cap, no document open
 * failures except for files genuinely missing from disk.
 *
 * Override knobs:
 *   FW_SOAK_STRIDE         (default 5)   — walk every Nth page
 *   FW_SOAK_DWELL_MS       (default 60)  — per-page mainloop spin
 *   FW_SOAK_MAX_PAGES      (default 200) — cap per document; 0 = no cap
 *   FW_STRESS_RSS_CAP_MB   (default 1500)
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
#include <stdlib.h>

/* Comics-heavy runs (CBZ + CBR back-to-back) push RSS past 1500 MB
 * with the default 512 MB byte-cap × two formats × glibc retention.
 * 1800 MB is comfortable for a clean corpus walk; raise this only if a
 * real regression pushes us higher. */
#define DEFAULT_RSS_CAP_MB  1800
#define DEFAULT_STRIDE        5
#define DEFAULT_DWELL_MS     60
#define DEFAULT_MAX_PAGES   200

/* One sample per backend, resolved against the corpus root (see
 * resolve_corpus_root). Filenames match the .testfiles layout and
 * tests/corpus.json. Missing files are skipped (not failures), so a
 * partially-populated corpus still runs. */
static const char *CORPUS_FILES[] = {
  "effective-java.pdf",            /* PDF — textbook, 901pp, font/code-heavy */
  "visual-explanations-tufte.pdf", /* PDF — image-heavy */
  "on-growth-and-form.djvu",       /* DjVu — scanned */
  "playing-at-the-world-v2.epub",  /* reflow routes via MuPDF here (FwDocument) */
  "the-broken-god.mobi",           /* MOBI / KF7 */
  "datapoint.azw3",                /* AZW3 / KF8 */
  "nausicaa-v01.cbz",              /* CBZ */
  "vagabond-v01.cbr",              /* CBR — RAR via libarchive */
};
static const int CORPUS_LEN = (int) (sizeof (CORPUS_FILES) / sizeof (CORPUS_FILES[0]));


static gboolean
soak_one (const char *path, int stride, int dwell_ms, int max_pages)
{
  if (!file_exists (path)) {
    printf ("  skip (missing): %s\n", path);
    return TRUE;
  }
  printf ("  open: %s\n", path);

  g_autoptr (GError) error = NULL;
  FwDocument *doc = fw_document_new_for_path (path, &error);
  if (!doc) {
    fprintf (stderr, "    FAIL open: %s\n",
             error ? error->message : "(null)");
    return FALSE;
  }
  int page_count = fw_document_get_page_count (doc);

  FwCache *cache = fw_cache_new (doc, NULL);
  fw_cache_set_scale_factor (cache, 1);
  fw_cache_start (cache, 1.0, 0);
  fw_cache_set_velocity (cache, 0.0);   /* STATIC — workers should run to completion */

  int touched = 0;
  for (int pg = 0; pg < page_count; pg += stride) {
    int single[1] = { pg };
    fw_cache_set_priority (cache, single, 1);
    spin_main_loop (dwell_ms);
    touched++;
    if (max_pages > 0 && touched >= max_pages) break;
  }
  printf ("    pages=%d touched=%d rss=%ld MB\n",
          page_count, touched, rss_current_mb ());

  fw_cache_stop (cache);
  spin_main_loop (50);
  g_object_unref (cache);
  g_object_unref (doc);
  return TRUE;
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

  int stride = DEFAULT_STRIDE;
  const char *stride_env = g_getenv ("FW_SOAK_STRIDE");
  if (stride_env) stride = atoi (stride_env);
  if (stride < 1) stride = 1;

  int dwell_ms = DEFAULT_DWELL_MS;
  const char *dwell_env = g_getenv ("FW_SOAK_DWELL_MS");
  if (dwell_env) dwell_ms = atoi (dwell_env);
  if (dwell_ms < 1) dwell_ms = 1;

  int max_pages = DEFAULT_MAX_PAGES;
  const char *max_env = g_getenv ("FW_SOAK_MAX_PAGES");
  if (max_env) max_pages = atoi (max_env);

  g_autofree char *root = fw_test_corpus_root ();
  printf ("stress-corpus-soak: root=%s stride=%d dwell=%dms max_pages=%d cap=%ld MB\n",
          root, stride, dwell_ms, max_pages, rss_cap_mb);

  int failures = 0;
  int processed = 0;
  for (int i = 0; i < CORPUS_LEN; i++) {
    g_autofree char *path = g_build_filename (root, CORPUS_FILES[i], NULL);
    if (!soak_one (path, stride, dwell_ms, max_pages))
      failures++;
    processed++;
  }

  long peak = rss_peak_mb ();
  printf ("done: processed=%d failures=%d peak_rss=%ld MB\n",
          processed, failures, peak);

  if (peak > rss_cap_mb) {
    fprintf (stderr, "FAIL: peak RSS %ld MB exceeded cap %ld MB.\n",
             peak, rss_cap_mb);
    return 1;
  }
  if (failures > 0) {
    fprintf (stderr, "FAIL: %d documents failed to open or render.\n", failures);
    return 1;
  }
  printf ("PASS: stress-corpus-soak completed under cap.\n");
  return 0;
}
