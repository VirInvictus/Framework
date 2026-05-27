/* tests/stress/bench-render.c — render-latency benchmark
 *
 * Measures the cost of `fw_document_render_page` across a configurable
 * span of pages, in two passes:
 *
 *   - cold: fresh document handle, no warm caches — the first time the
 *     viewer touches a page after open.
 *   - warm: same document handle, second pass — exercises MuPDF's font
 *     and image stores (per-instance store size from v0.26 Tier 3).
 *
 * Reports p50 / p95 / p99 / max in microseconds, plus mean and total.
 * The cache layer is intentionally bypassed — this benchmark answers
 * "how fast does the backend render?" not "how well does the cache
 * hide latency?" For the cache view, run stress-scrub with FW_DEBUG=1.
 *
 * Usage:
 *   bench-render <doc>                 # 50 evenly-spaced pages, zoom 1.0
 *   bench-render <doc> --pages 100     # 100 evenly-spaced pages
 *   bench-render <doc> --zoom 1.5      # render at 1.5×
 *   bench-render <doc> --stride 5      # pages 0, 5, 10, …
 *
 * Without an argument, falls back to the default corpus sample.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-debug.h"

#include <gtk/gtk.h>
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SAMPLE_COUNT 50
#define DEFAULT_ZOOM         1.0

static int
gint64_compare (gconstpointer a, gconstpointer b)
{
  gint64 va = *(const gint64 *) a;
  gint64 vb = *(const gint64 *) b;
  if (va < vb) return -1;
  if (va > vb) return  1;
  return 0;
}

static gint64
percentile (const GArray *sorted, double p)
{
  if (sorted->len == 0) return 0;
  guint idx = (guint) (p * (double) (sorted->len - 1) + 0.5);
  if (idx >= sorted->len) idx = sorted->len - 1;
  return g_array_index (sorted, gint64, idx);
}

static void
report (const char *label, GArray *samples_us)
{
  if (samples_us->len == 0) {
    printf ("  %s: (no samples)\n", label);
    return;
  }
  g_array_sort (samples_us, gint64_compare);

  gint64 total = 0, mx = 0;
  for (guint i = 0; i < samples_us->len; i++) {
    gint64 v = g_array_index (samples_us, gint64, i);
    total += v;
    if (v > mx) mx = v;
  }
  double mean_ms = (double) total / (double) samples_us->len / 1000.0;
  double p50_ms  = (double) percentile (samples_us, 0.50) / 1000.0;
  double p95_ms  = (double) percentile (samples_us, 0.95) / 1000.0;
  double p99_ms  = (double) percentile (samples_us, 0.99) / 1000.0;
  double max_ms  = (double) mx / 1000.0;
  double tot_s   = (double) total / 1e6;

  printf ("  %-6s  n=%-4u  mean=%7.2f ms  p50=%7.2f ms  p95=%7.2f ms  p99=%7.2f ms  max=%7.2f ms  total=%6.2f s\n",
          label, samples_us->len, mean_ms, p50_ms, p95_ms, p99_ms, max_ms, tot_s);
}

static void
run_pass (FwDocument *doc, const int *pages, int n_pages, double zoom,
          const char *label)
{
  GArray *samples = g_array_sized_new (FALSE, FALSE, sizeof (gint64), n_pages);
  for (int i = 0; i < n_pages; i++) {
    gint64 t0 = g_get_monotonic_time ();
    cairo_surface_t *surf = fw_document_render_page (doc, pages[i], zoom, 0);
    gint64 t1 = g_get_monotonic_time ();
    if (surf) {
      gint64 dt = t1 - t0;
      g_array_append_val (samples, dt);
      cairo_surface_destroy (surf);
    } else {
      fprintf (stderr, "  %s: render failed at page %d\n", label, pages[i]);
    }
  }
  report (label, samples);
  g_array_unref (samples);
}

int
main (int argc, char **argv)
{
  fw_debug_init ();
  gtk_init ();

  const char *path = NULL;
  int sample_count = DEFAULT_SAMPLE_COUNT;
  int stride = 0;            /* 0 = "evenly spaced over doc" */
  double zoom = DEFAULT_ZOOM;

  for (int i = 1; i < argc; i++) {
    if (strcmp (argv[i], "--pages") == 0 && i + 1 < argc) {
      sample_count = atoi (argv[++i]);
    } else if (strcmp (argv[i], "--zoom") == 0 && i + 1 < argc) {
      zoom = atof (argv[++i]);
    } else if (strcmp (argv[i], "--stride") == 0 && i + 1 < argc) {
      stride = atoi (argv[++i]);
    } else if (argv[i][0] != '-') {
      path = argv[i];
    }
  }

  if (!path) {
    fprintf (stderr,
             "bench-render: pass a document path as argv[1].\n"
             "  example: bench-render .testfiles/effective-java.pdf\n");
    return 2;
  }

  printf ("bench-render: %s\n", path);
  printf ("  zoom=%.2f  pages=%d  stride=%d\n",
          zoom, sample_count, stride);

  g_autoptr (GError) error = NULL;
  FwDocument *doc = fw_document_new_for_path (path, &error);
  if (!doc) {
    fprintf (stderr, "bench-render: open failed: %s\n",
             error ? error->message : "(null)");
    return 1;
  }
  int total_pages = fw_document_get_page_count (doc);
  printf ("  doc has %d pages\n", total_pages);

  /* Build the page list. If stride is set, walk by stride; otherwise
   * spread sample_count evenly across the document. */
  GArray *page_list = g_array_new (FALSE, FALSE, sizeof (int));
  if (stride > 0) {
    for (int p = 0; p < total_pages; p += stride)
      g_array_append_val (page_list, p);
  } else {
    int n = sample_count > total_pages ? total_pages : sample_count;
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) {
      int p = (int) ((gint64) i * (gint64) (total_pages - 1) / (gint64) (n > 1 ? n - 1 : 1));
      g_array_append_val (page_list, p);
    }
  }
  int *pages = (int *) page_list->data;
  int n_pages = (int) page_list->len;
  printf ("  rendering %d pages\n\n", n_pages);

  run_pass (doc, pages, n_pages, zoom, "cold");
  run_pass (doc, pages, n_pages, zoom, "warm");

  g_array_unref (page_list);
  g_object_unref (doc);
  return 0;
}
