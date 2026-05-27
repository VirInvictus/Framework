/* tests/stress/stress-reflow.c — reflow pipeline regression test
 *
 * Every other stress test drives FwDocument + FwCache, i.e. the
 * fixed-layout (MuPDF / DjVu / libarchive) pipeline. None of them touch
 * the native reflow pipeline: the EPUB / MOBI / AZW3 parsers behind
 * FwReflowDocument, or the v0.66 search core. This test does.
 *
 * Scope is the document layer only. Pagination and search-highlight
 * splicing live in FwReflowView, a GtkWidget — exercising those means
 * realizing a widget, which needs a display, so they're verified by
 * running the app rather than here. What this catches: parser
 * regressions (empty / malformed block models), search-core breakage
 * (wrong hit counts, out-of-range block indices, leaks), and ownership
 * bugs in the get_toc / get_metadata accessors. ASan reports leaks;
 * the harness reports crashes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-document.h"
#include "corpus-root.h"

#include <glib.h>
#include <stdio.h>

typedef struct {
  const char *file;
  const char *needle;   /* a word that should appear in normal prose */
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

  /* Walk every block, touching each accessor. Catches parser output that
   * is structurally bad in a way that only blows up on read. */
  GListModel *model = fw_reflow_document_get_block_model (doc);
  guint n = model ? g_list_model_get_n_items (model) : 0;
  guint nonempty = 0;
  for (guint i = 0; i < n; i++) {
    g_autoptr (FwBlock) b = g_list_model_get_item (model, i);
    (void) fw_block_get_kind (b);
    (void) fw_block_get_level (b);
    (void) fw_block_get_image_id (b);
    (void) fw_block_get_anchor_id (b);
    (void) fw_block_get_flags (b);
    const char *t = fw_block_get_text (b);
    if (t && *t) nonempty++;
  }
  if (n == 0) {
    fprintf (stderr, "  FAIL %s: empty block model\n", s->file);
    fail = 1;
  }

  /* Search: a common word should hit, and every hit must reference a
   * real block. */
  GArray *hits = fw_reflow_document_search (doc, s->needle);
  guint hit_count = hits ? hits->len : 0;
  if (hits) {
    for (guint i = 0; i < hits->len; i++) {
      FwReflowHit *h = &g_array_index (hits, FwReflowHit, i);
      if (h->block >= n) {
        fprintf (stderr, "  FAIL %s: hit block %u out of range (n=%u)\n",
                 s->file, h->block, n);
        fail = 1;
        break;
      }
    }
    g_array_unref (hits);
  }
  if (nonempty > 0 && hit_count == 0)
    fprintf (stderr, "  WARN %s: '%s' produced no hits in %u non-empty blocks\n",
             s->file, s->needle, nonempty);

  /* A garbage token must return no matches, and an empty needle must
   * return NULL (not an empty array). */
  GArray *none = fw_reflow_document_search (doc, "zzqqxx_no_such_token_42");
  if (none) {
    fprintf (stderr, "  FAIL %s: garbage token returned %u hits\n",
             s->file, none->len);
    fail = 1;
    g_array_unref (none);
  }
  GArray *empty = fw_reflow_document_search (doc, "");
  if (empty) {
    fprintf (stderr, "  FAIL %s: empty needle returned non-NULL\n", s->file);
    fail = 1;
    g_array_unref (empty);
  }

  /* TOC (transfer-none, do not unref) + metadata (transfer-full, unref)
   * smoke: just call them and let ASan catch ownership bugs. */
  (void) fw_reflow_document_get_toc (doc);
  GHashTable *meta = fw_reflow_document_get_metadata (doc);
  if (meta) g_hash_table_unref (meta);

  printf ("  %s: blocks=%u (nonempty=%u) hits['%s']=%u %s\n",
          s->file, n, nonempty, s->needle, hit_count, fail ? "FAIL" : "ok");

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
