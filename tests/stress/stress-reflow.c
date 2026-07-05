/* tests/stress/stress-reflow.c — reflow pipeline regression test
 *
 * Every other stress test drives FwDocument + FwCache, i.e. the
 * fixed-layout (MuPDF / DjVu / libarchive) pipeline. None of them touch
 * the reflow pipeline: the EPUB / MOBI / AZW3 parsers behind
 * FwReflowDocument. This test does.
 *
 * Scope is the document layer only. Reflow formats render in a
 * WebKitWebView (FwWebView), which needs a display, so the rendered
 * output is verified by running the app rather than here. What this
 * catches: parser regressions in produce_html (empty / structurally
 * broken HTML), ownership bugs in the image table (the per-load
 * GHashTable<gchar*, GBytes*> handed to the WebView), and ownership bugs
 * in the get_toc / get_metadata accessors. ASan reports leaks; the
 * harness reports crashes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document.h"
#include "corpus-root.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char *file;
  const char *needle;   /* a lowercase word that should appear in prose */
} ReflowSample;

static const ReflowSample SAMPLES[] = {
  { "playing-at-the-world-v2.epub", "the" },  /* EPUB */
  { "the-broken-god.mobi",          "the" },  /* MOBI / KF7 */
  { "datapoint.azw3",               "the" },  /* AZW3 / KF8 */
};
static const int N_SAMPLES = (int) (sizeof (SAMPLES) / sizeof (SAMPLES[0]));

/* Returns the number of failures for this sample (0 = pass). A missing
 * file is skipped, not a failure, so a partial corpus still runs. */
static int
test_one (const char *root, const ReflowSample *s)
{
  g_autofree char *path = g_build_filename (root, s->file, NULL);
  if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
    printf ("  skip (missing): %s\n", s->file);
    return 0;
  }

  g_autoptr (GError) error = NULL;
  FwReflowDocument *doc = fw_reflow_document_new_for_path (path, &error);
  if (!doc) {
    fprintf (stderr, "  FAIL open %s: %s\n", s->file,
             error ? error->message : "(null)");
    return 1;
  }

  int fail = 0;

  /* produce_html is the live render path: parse the whole document and
   * stitch it into one HTML string plus an image table. */
  char *html = NULL;
  GHashTable *images = NULL;
  if (!fw_reflow_document_produce_html (doc, "stress", &html, &images, &error)) {
    fprintf (stderr, "  FAIL produce_html %s: %s\n", s->file,
             error ? error->message : "(null)");
    g_object_unref (doc);
    return 1;
  }

  gsize html_len = html ? strlen (html) : 0;
  if (html_len == 0) {
    fprintf (stderr, "  FAIL %s: produce_html returned empty HTML\n", s->file);
    fail = 1;
  } else {
    /* Structural sanity: a stitched document has a body and closes. */
    if (!strstr (html, "<body") || !strstr (html, "</html>")) {
      fprintf (stderr, "  FAIL %s: HTML missing <body>/</html>\n", s->file);
      fail = 1;
    }
    /* Content smoke: a common word should survive into the rendered
     * HTML. Soft (WARN) — case / markup can hide it in edge cases. */
    g_autofree char *low = g_ascii_strdown (html, -1);
    if (!strstr (low, s->needle))
      fprintf (stderr, "  WARN %s: needle '%s' not found in HTML\n",
               s->file, s->needle);
  }

  /* The image table is optional, but when present every value must be a
   * non-empty GBytes (catches type / ownership bugs in the resolver). */
  guint n_images = 0;
  if (images) {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, images);
    while (g_hash_table_iter_next (&it, &k, &v)) {
      GBytes *b = v;
      if (!b || g_bytes_get_size (b) == 0) {
        fprintf (stderr, "  FAIL %s: image '%s' has no bytes\n",
                 s->file, (const char *) k);
        fail = 1;
        break;
      }
      n_images++;
    }
    g_hash_table_unref (images);
  }
  g_free (html);

  /* TOC (transfer-none, do not unref) + metadata (transfer-full, unref)
   * smoke: just call them and let ASan catch ownership bugs. */
  (void) fw_reflow_document_get_toc (doc);
  GHashTable *meta = fw_reflow_document_get_metadata (doc);
  if (meta) g_hash_table_unref (meta);

  printf ("  %s: html=%zu bytes images=%u %s\n",
          s->file, html_len, n_images, fail ? "FAIL" : "ok");

  g_object_unref (doc);
  return fail;
}

int
main (int argc, char **argv)
{
  (void) argc; (void) argv;

  g_autofree char *root = fw_test_corpus_root ();
  printf ("stress-reflow: root=%s\n", root);

  int failures = 0;
  int processed = 0;
  for (int i = 0; i < N_SAMPLES; i++) {
    failures += test_one (root, &SAMPLES[i]);
    processed++;
  }

  printf ("done: processed=%d failures=%d\n", processed, failures);
  if (failures > 0) {
    fprintf (stderr, "FAIL: %d reflow check(s) failed.\n", failures);
    return 1;
  }
  printf ("PASS: stress-reflow completed.\n");
  return 0;
}
