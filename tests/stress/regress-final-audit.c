/* regress-final-audit.c — Regression net for THE FINAL AUDIT (2026-09-13)
 *
 * Document-layer tests (no display); synthetic documents are built in a
 * temp dir, so no corpus is needed:
 *
 *   1. PDF metadata clamp (HIGH): fz_lookup_metadata returns the
 *      UNTRUNCATED size per the MuPDF header; metadata_lookup must
 *      clamp it before trimming or a crafted PDF with a Title/Author/
 *      Keywords longer than 1023 bytes walks past the 1024-byte stack
 *      buffer via Document Properties.
 *   2. DjVu lock discipline (HIGH): the toc/text/links/search queries
 *      run against the shared ddjvu context under render_lock, racing
 *      render workers on the corpus DjVu (skipped when the corpus file
 *      is absent).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "corpus-root.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* ── 1. PDF metadata clamp ─────────────────────────────────────────── */

/* Minimal one-page PDF whose Info dictionary carries a Title of
 * `title_len` bytes and an Author of `author_len` bytes. Xref offsets
 * are tracked as the objects are appended. */
static char *
write_pdf_with_long_info (const char *dir, const char *filename,
                          gsize title_len, gsize author_len)
{
  GString *s = g_string_new (NULL);
  gsize off[5] = { 0, 0, 0, 0, 0 };

  g_string_append (s, "%PDF-1.4\n");

  off[1] = s->len;
  g_string_append (s, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

  off[2] = s->len;
  g_string_append (s,
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");

  off[3] = s->len;
  g_string_append (s,
    "3 0 obj\n<< /Type /Page /Parent 2 0 R "
    "/MediaBox [0 0 200 200] >>\nendobj\n");

  off[4] = s->len;
  g_string_append (s, "4 0 obj\n<< /Title (");
  for (gsize i = 0; i < title_len; i++)
    g_string_append_c (s, 'A' + (i % 26));
  g_string_append (s, ") /Author (");
  for (gsize i = 0; i < author_len; i++)
    g_string_append_c (s, 'a' + (i % 26));
  g_string_append (s, ") >>\nendobj\n");

  gsize xref_off = s->len;
  g_string_append (s, "xref\n0 5\n0000000000 65535 f \n");
  for (int i = 1; i <= 4; i++)
    g_string_append_printf (s, "%010zu 00000 n \n", off[i]);
  g_string_append_printf (s,
    "trailer\n<< /Size 5 /Root 1 0 R /Info 4 0 R >>\n"
    "startxref\n%zu\n%%%%EOF\n", xref_off);

  char *path = g_build_filename (dir, filename, NULL);
  GError *error = NULL;
  g_file_set_contents (path, s->str, s->len, &error);
  g_assert_no_error (error);
  g_string_free (s, TRUE);
  return path;
}

static FwDocument *
open_doc (const char *path)
{
  GError *error = NULL;
  FwDocument *doc = fw_document_new_for_path (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (doc);
  return doc;
}

/* The crafted case: Title/Author far past the 1024-byte lookup buffer.
 * The metadata arrives truncated to 1023 bytes and the trim loop stays
 * inside the buffer (ASan flags the overrun the moment it does not). */
static void
test_pdf_metadata_long_values (const char *dir)
{
  g_test_message ("pdf: metadata longer than the 1024-byte lookup buffer");

  char *path = write_pdf_with_long_info (dir, "longinfo.pdf", 2048, 1536);
  FwDocument *doc = open_doc (path);

  GHashTable *meta = fw_document_get_metadata (doc);
  g_assert_nonnull (meta);

  const char *title = g_hash_table_lookup (meta, "title");
  g_assert_nonnull (title);
  g_assert_cmpuint (strlen (title), ==, 1023);
  g_assert_cmpint (title[0], ==, 'A');

  const char *author = g_hash_table_lookup (meta, "author");
  g_assert_nonnull (author);
  g_assert_cmpuint (strlen (author), ==, 1023);

  g_hash_table_unref (meta);
  g_object_unref (doc);
  g_free (path);
}

/* The ordinary case: short values round-trip whole, so the clamp only
 * ever bites past the buffer size. */
static void
test_pdf_metadata_short_values (const char *dir)
{
  g_test_message ("pdf: short metadata still round-trips");

  char *path = write_pdf_with_long_info (dir, "shortinfo.pdf", 8, 11);
  FwDocument *doc = open_doc (path);

  GHashTable *meta = fw_document_get_metadata (doc);
  g_assert_nonnull (meta);

  const char *title = g_hash_table_lookup (meta, "title");
  g_assert_nonnull (title);
  g_assert_cmpstr (title, ==, "ABCDEFGH");

  const char *author = g_hash_table_lookup (meta, "author");
  g_assert_nonnull (author);
  g_assert_cmpstr (author, ==, "abcdefghijk");

  g_hash_table_unref (meta);
  g_object_unref (doc);
  g_free (path);
}

/* ── 2. DjVu queries under the render lock ─────────────────────────── */

typedef struct {
  FwDocument *doc;
  int pages_done;
} DjvuRenderSpy;

static void *
djvu_render_thread (gpointer data)
{
  DjvuRenderSpy *spy = data;
  for (int i = 0; i < 8; i++) {
    cairo_surface_t *s = fw_document_render_page (spy->doc, i % 3, 1.0, 0);
    if (s)
      cairo_surface_destroy (s);
    spy->pages_done++;
  }
  return NULL;
}

/* The old code called the outline/pagetext/pageanno/pageinfo queries
 * with no render_lock, racing the render workers on the shared (and
 * non-thread-safe) ddjvu context; the text queries also pump the
 * context message queue concurrently with renders. Drive every query
 * path while a render worker spins. */
static void
test_djvu_locked_queries (const char *path)
{
  g_test_message ("djvu: toc/text/links/search hold render_lock");

  FwDocument *doc = open_doc (path);
  g_assert_cmpint (fw_document_get_page_count (doc), >, 0);

  FwTocNode *toc = fw_document_get_toc (doc);
  fw_toc_node_free (toc);

  DjvuRenderSpy spy = { .doc = doc, .pages_done = 0 };
  GThread *worker = g_thread_new ("djvu-render", djvu_render_thread, &spy);
  for (int page = 0; page < 3; page++) {
    char *text = fw_document_get_text (doc, page, 0, 0, 0, 0);
    g_free (text);

    GArray *links = fw_document_get_links (doc, page);
    if (links)
      g_array_unref (links);

    GArray *hits = fw_document_search (doc, "e", page);
    if (hits)
      g_array_unref (hits);
  }
  g_thread_join (worker);
  g_assert_cmpint (spy.pages_done, ==, 8);

  g_object_unref (doc);
}

static void
test_djvu_wrap (void)
{
  g_autofree char *root = fw_test_corpus_root ();
  g_autofree char *path = g_build_filename (root, "on-growth-and-form.djvu",
                                            NULL);
  if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
    g_test_skip ("corpus .testfiles/on-growth-and-form.djvu not populated");
    return;
  }
  test_djvu_locked_queries (path);
}

/* ── Harness ───────────────────────────────────────────────────────── */

typedef struct {
  const char *dir;
} Fixture;

static Fixture fixture;

static void
setup (void)
{
  GError *error = NULL;
  fixture.dir = g_dir_make_tmp ("fw-finalaudit-XXXXXX", &error);
  g_assert_no_error (error);
}

static void
teardown (void)
{
  const char *files[] = { "longinfo.pdf", "shortinfo.pdf" };
  for (size_t i = 0; i < G_N_ELEMENTS (files); i++) {
    char *path = g_build_filename (fixture.dir, files[i], NULL);
    g_remove (path);
    g_free (path);
  }
  g_rmdir (fixture.dir);
}

static void
test_pdf_long_wrap (void)   { test_pdf_metadata_long_values (fixture.dir); }
static void
test_pdf_short_wrap (void)  { test_pdf_metadata_short_values (fixture.dir); }

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  setup ();
  g_test_add_func ("/final-audit/pdf-metadata-clamp", test_pdf_long_wrap);
  g_test_add_func ("/final-audit/pdf-metadata-roundtrip", test_pdf_short_wrap);
  g_test_add_func ("/final-audit/djvu-locked-queries", test_djvu_wrap);
  int status = g_test_run ();
  teardown ();
  return status;
}
