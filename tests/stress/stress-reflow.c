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
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include <archive.h>
#include <archive_entry.h>

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

/* ── Active-content scrub (synthetic hostile EPUB) ──────────────────
 *
 * fw_reflow_html_process must strip <script>/<iframe>/<object>/<embed>,
 * inline on* handlers, and javascript:-scheme URLs from ebook markup —
 * the WebView runs with JavaScript enabled, so any survivor executes.
 * The fixture is built in-test (libarchive zip writer, same library the
 * EPUB backend reads with), so this section needs no corpus. */

static gboolean
zip_add (struct archive *a, const char *name, const char *data)
{
  size_t len = strlen (data);
  struct archive_entry *e = archive_entry_new ();
  archive_entry_set_pathname (e, name);
  archive_entry_set_size (e, (la_int64_t) len);
  archive_entry_set_filetype (e, AE_IFREG);
  archive_entry_set_perm (e, 0644);
  gboolean ok = archive_write_header (a, e) == ARCHIVE_OK &&
                archive_write_data (a, data, len) == (la_ssize_t) len;
  archive_entry_free (e);
  return ok;
}

static const char HOSTILE_CONTAINER[] =
  "<?xml version=\"1.0\"?>\n"
  "<container version=\"1.0\" "
    "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
  "  <rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
  "</container>\n";

static const char HOSTILE_OPF[] =
  "<?xml version=\"1.0\"?>\n"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
    "unique-identifier=\"uid\">\n"
  "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
  "    <dc:identifier id=\"uid\">urn:uuid:fw-stress-hostile</dc:identifier>\n"
  "    <dc:title>Hostile Markup Fixture</dc:title>\n"
  "    <dc:language>en</dc:language>\n"
  "  </metadata>\n"
  "  <manifest>\n"
  "    <item id=\"ch1\" href=\"ch1.xhtml\" "
    "media-type=\"application/xhtml+xml\"/>\n"
  "  </manifest>\n"
  "  <spine><itemref idref=\"ch1\"/></spine>\n"
  "</package>\n";

static const char HOSTILE_CHAPTER[] =
  "<html><head><title>hostile</title></head><body>\n"
  "<p onclick=\"alert(1)\">Benign paragraph survives.</p>\n"
  "<p><em>inline kept</em></p>\n"
  "<img src=\"missing.png\" onerror=\"alert(2)\" onload=\"alert(3)\">\n"
  "<a href=\"javascript:alert(4)\">plain</a>\n"
  "<a href=\"java&#x0A;script:alert(5)\">split scheme</a>\n"
  "<a href=\"  JaVaScRiPt:alert(6)\">cased + padded</a>\n"
  "<a href=\"https://example.org/ok\">normal link kept</a>\n"
  "<iframe src=\"https://evil.example/\"></iframe>\n"
  "<object data=\"x.swf\"></object>\n"
  "<embed src=\"x.swf\">\n"
  "<script>alert(7)</script>\n"
  "</body></html>\n";

static int
check_absent (const char *low, const char *needle, const char *what)
{
  if (strstr (low, needle)) {
    fprintf (stderr, "  FAIL scrub: %s survived ('%s' found in HTML)\n",
             what, needle);
    return 1;
  }
  return 0;
}

static int
test_scrub (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *dir = g_dir_make_tmp ("fw-stress-reflow-XXXXXX", &error);
  if (!dir) {
    fprintf (stderr, "  FAIL scrub: tmp dir: %s\n",
             error ? error->message : "(null)");
    return 1;
  }
  g_autofree char *path = g_build_filename (dir, "hostile.epub", NULL);

  struct archive *a = archive_write_new ();
  archive_write_set_format_zip (a);
  gboolean wrote =
    archive_write_open_filename (a, path) == ARCHIVE_OK &&
    zip_add (a, "mimetype", "application/epub+zip") &&
    zip_add (a, "META-INF/container.xml", HOSTILE_CONTAINER) &&
    zip_add (a, "OEBPS/content.opf", HOSTILE_OPF) &&
    zip_add (a, "OEBPS/ch1.xhtml", HOSTILE_CHAPTER);
  archive_write_free (a);
  if (!wrote) {
    fprintf (stderr, "  FAIL scrub: could not write fixture zip\n");
    return 1;
  }

  int fail = 0;
  FwReflowDocument *doc = fw_reflow_document_new_for_path (path, &error);
  char *html = NULL;
  GHashTable *images = NULL;
  if (!doc ||
      !fw_reflow_document_produce_html (doc, "scrub", &html, &images, &error)) {
    fprintf (stderr, "  FAIL scrub: open/produce_html: %s\n",
             error ? error->message : "(null)");
    fail = 1;
  } else {
    g_autofree char *low = g_ascii_strdown (html, -1);
    fail += check_absent (low, "<script",     "script element");
    fail += check_absent (low, "<iframe",     "iframe element");
    fail += check_absent (low, "<object",     "object element");
    fail += check_absent (low, "<embed",      "embed element");
    fail += check_absent (low, "onclick",     "onclick handler");
    fail += check_absent (low, "onerror",     "onerror handler");
    fail += check_absent (low, "onload",      "onload handler");
    fail += check_absent (low, "javascript",  "javascript: URL");
    fail += check_absent (low, "alert(",      "script payload");
    if (!strstr (low, "benign paragraph survives")) {
      fprintf (stderr, "  FAIL scrub: benign prose was lost\n");
      fail++;
    }
    if (!strstr (low, "https://example.org/ok")) {
      fprintf (stderr, "  FAIL scrub: benign href was lost\n");
      fail++;
    }
  }
  g_free (html);
  if (images) g_hash_table_unref (images);
  if (doc) g_object_unref (doc);

  (void) g_unlink (path);
  (void) g_rmdir (dir);

  printf ("  hostile.epub (synthetic): %s\n", fail ? "FAIL" : "ok");
  return fail ? 1 : 0;
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

  failures += test_scrub ();
  processed++;

  printf ("done: processed=%d failures=%d\n", processed, failures);
  if (failures > 0) {
    fprintf (stderr, "FAIL: %d reflow check(s) failed.\n", failures);
    return 1;
  }
  printf ("PASS: stress-reflow completed.\n");
  return 0;
}
