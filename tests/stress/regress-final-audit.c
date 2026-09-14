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
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"

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
  int status = g_test_run ();
  teardown ();
  return status;
}
