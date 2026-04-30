/* tests/stress/stress-search-cache.c — validates the stext cache path
 *
 * Runs a search query across every page of a document twice. The first
 * pass populates the per-page fz_stext_page cache (cold); the second
 * pass should hit cache for every page (warm). Asserts the warm pass
 * is meaningfully faster — if the cache is doing its job, it's a 2-5×
 * speedup on a textbook-sized doc.
 *
 * Without a working cache, the second pass would re-extract stext for
 * every page and run at the same speed as the first. This test fails
 * loudly in that case.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-debug.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

static double
elapsed_ms (gint64 from)
{
  return (g_get_monotonic_time () - from) / 1000.0;
}

/* Run a full-document search by walking every page and counting hits. */
static int
search_all_pages (FwDocument *doc, int page_count, const char *query)
{
  int total_hits = 0;
  for (int p = 0; p < page_count; p++) {
    GArray *hits = fw_document_search (doc, query, p);
    if (hits) {
      total_hits += hits->len;
      g_array_unref (hits);
    }
  }
  return total_hits;
}

int
main (int argc, char **argv)
{
  fw_debug_init ();
  gtk_init ();

  if (argc < 3) {
    fprintf (stderr, "stress-search-cache: usage: stress-search-cache <doc> <query>\n"
                     "  example: stress-search-cache effective-java.pdf 'class'\n");
    return 2;
  }
  const char *path  = argv[1];
  const char *query = argv[2];

  printf ("stress-search-cache: opening %s\n", path);
  g_autoptr (GError) error = NULL;
  FwDocument *doc = fw_document_new_for_path (path, &error);
  if (!doc) {
    fprintf (stderr, "open failed: %s\n", error ? error->message : "(null)");
    return 1;
  }
  int page_count = fw_document_get_page_count (doc);
  printf ("stress-search-cache: %d pages, query=\"%s\"\n", page_count, query);

  /* Cold pass — every page extracts stext for the first time. */
  gint64 t0 = g_get_monotonic_time ();
  int cold_hits = search_all_pages (doc, page_count, query);
  double cold_ms = elapsed_ms (t0);
  printf ("cold pass:  %d hits in %.1f ms (%.2f ms/page)\n",
          cold_hits, cold_ms, cold_ms / page_count);

  /* Warm pass — every page should hit cache. */
  t0 = g_get_monotonic_time ();
  int warm_hits = search_all_pages (doc, page_count, query);
  double warm_ms = elapsed_ms (t0);
  printf ("warm pass:  %d hits in %.1f ms (%.2f ms/page)\n",
          warm_hits, warm_ms, warm_ms / page_count);

  if (cold_hits != warm_hits) {
    fprintf (stderr, "FAIL: hit count differs between cold (%d) and warm (%d).\n",
             cold_hits, warm_hits);
    g_object_unref (doc);
    return 1;
  }

  /* The cache should give at least a 1.5× speedup on a doc this size.
   * In practice it's much higher (typically 3-10× on textbook-sized
   * PDFs) — the threshold is generous to avoid flakiness from system
   * jitter. The point is "cache must measurably help"; tighter perf
   * regression detection would belong in bench-search (Phase 12.3). */
  double speedup = cold_ms / warm_ms;
  printf ("speedup:    %.2fx\n", speedup);

  g_object_unref (doc);

  if (speedup < 1.5) {
    fprintf (stderr, "FAIL: warm pass is not meaningfully faster than cold "
                     "(speedup %.2fx, expected ≥1.5x).\n", speedup);
    return 1;
  }
  printf ("PASS: stext cache delivers %.2fx speedup on warm searches.\n", speedup);
  return 0;
}
