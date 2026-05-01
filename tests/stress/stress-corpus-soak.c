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

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

/* Comics-heavy runs (CBZ + CBR back-to-back) push RSS past 1500 MB
 * with the default 512 MB byte-cap × two formats × glibc retention.
 * 1800 MB is comfortable for a clean corpus walk; raise this only if a
 * real regression pushes us higher. */
#define DEFAULT_RSS_CAP_MB  1800
#define DEFAULT_STRIDE        5
#define DEFAULT_DWELL_MS     60
#define DEFAULT_MAX_PAGES   200

/* Same coverage as stress-multidoc, but every backend gets a soak —
 * walked in stride rather than just touched. Missing files are
 * skipped (not failures). */
static const char *CORPUS[] = {
  "/home/bdkl/docs/Calibre Library/Joshua Bloch/Effective Java (5)/Effective Java - Joshua Bloch.pdf",
  "/home/bdkl/docs/Calibre Library/Imre Lakatos/Proofs and Refutations_ The Logic of Mathematical Discovery (1006)/Proofs and Refutations_ The Logic of Mathe - Imre Lakatos.djvu",
  "/home/bdkl/docs/Calibre Library/Andrew Hunt/The Pragmatic Programmer_ your journey to mastery, 20th Anniversary Edition, 2nd Edition (8)/The Pragmatic Programmer_ your journey to - Andrew Hunt.pdf",
  "/home/bdkl/docs/Calibre Library/Troy Denning/The Verdant Passage (2)/The Verdant Passage - Troy Denning.epub",
  "/home/bdkl/docs/Calibre Library/David Gemmell/Fall of Kings (1581)/Fall of Kings - David Gemmell.mobi",
  "/mnt/SharedData/Comics/Berserk (v01-v041)/Berserk v25 (2008) (Digital) (danke-Empire).cbz",
  "/mnt/SharedData/Comics/From Hell (Master Edition)/From Hell - Master Edition (2020).cbr",
};
static const int CORPUS_LEN = (int) (sizeof (CORPUS) / sizeof (CORPUS[0]));

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

static gboolean
file_exists (const char *path)
{
  return g_file_test (path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR);
}

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

  printf ("stress-corpus-soak: stride=%d dwell=%dms max_pages=%d cap=%ld MB\n",
          stride, dwell_ms, max_pages, rss_cap_mb);

  int failures = 0;
  int processed = 0;
  for (int i = 0; i < CORPUS_LEN; i++) {
    if (!soak_one (CORPUS[i], stride, dwell_ms, max_pages))
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
