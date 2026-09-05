/* regress-phase20.c — Regression net for the Phase 20 sweep
 *
 * Document-layer tests (no display, no corpus needed — the archives are
 * synthetic, built in a temp dir with libarchive's writer):
 *
 *   1. ComicInfo.xml metadata (Phase 20 growth): parse, archive
 *      extraction, CBR get_metadata, CBZ merge, absent-sidecar → NULL.
 *   2. CBR cancellation (Phase 20 critical bug): cancel_render during
 *      an open document must not permanently break rendering.
 *   3. Search indicator (Phase 20 bug): a scan that finishes on its
 *      own must clear fw_search_is_running.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-comicinfo.h"
#include "fw-search.h"
#include "fw-cache.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>
#include <string.h>

/* 2x2 red PNG (valid image for MuPDF's decoder, 76 bytes). */
static const guint8 RED_PNG[] = {
  0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
  0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x08,0x02,0x00,0x00,0x00,0xFD,0xD4,0x9A,
  0x73,0x00,0x00,0x00,0x13,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0xF8,0xCF,0xC0,0xF0,
  0x9F,0x01,0x8C,0xFF,0x33,0x30,0x00,0x00,0x1F,0xEE,0x03,0xFD,0x35,0x1B,0x00,0x33,
  0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82,
};

static const char COMICINFO_XML[] =
  "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
  "<ComicInfo xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
  " xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\n"
  "  <Title>The Test Issue</Title>\n"
  "  <Series>Regression City</Series>\n"
  "  <Number>20</Number>\n"
  "  <Volume>2026</Volume>\n"
  "  <Writer>Ada Author</Writer>\n"
  "  <Penciller>Pablo Penciller</Penciller>\n"
  "  <Publisher>Sweep Comics</Publisher>\n"
  "  <Genre>Testing</Genre>\n"
  "</ComicInfo>\n";

/* ── Synthetic archive writer (libarchive, zip format) ─────────────── */

static void
write_entry (struct archive *a, const char *name,
             const void *data, size_t len)
{
  struct archive_entry *entry = archive_entry_new ();
  archive_entry_set_pathname (entry, name);
  archive_entry_set_size (entry, (gint64) len);
  archive_entry_set_filetype (entry, AE_IFREG);
  archive_entry_set_perm (entry, 0644);
  if (archive_write_header (a, entry) != ARCHIVE_OK)
    g_error ("archive_write_header(%s): %s", name, archive_error_string (a));
  if (len > 0 && archive_write_data (a, data, len) != (la_ssize_t) len)
    g_error ("archive_write_data(%s): %s", name, archive_error_string (a));
  archive_entry_free (entry);
}

/* Writes a two-page zip comic; `with_info` adds a ComicInfo.xml. */
static char *
write_comic_zip (const char *dir, const char *filename, gboolean with_info)
{
  char *path = g_build_filename (dir, filename, NULL);
  struct archive *a = archive_write_new ();
  g_assert_nonnull (a);
  archive_write_set_format_zip (a);
  /* Sparseness options don't matter; just take the defaults. */
  if (archive_write_open_filename (a, path) != ARCHIVE_OK)
    g_error ("archive_write_open_filename(%s): %s",
             path, archive_error_string (a));

  write_entry (a, "page001.png", RED_PNG, sizeof (RED_PNG));
  write_entry (a, "page002.png", RED_PNG, sizeof (RED_PNG));
  if (with_info)
    write_entry (a, "ComicInfo.xml", COMICINFO_XML,
                 strlen (COMICINFO_XML));

  if (archive_write_close (a) != ARCHIVE_OK ||
      archive_write_free (a) != ARCHIVE_OK)
    g_error ("archive write failed: %s", archive_error_string (a));
  return path;
}

/* ── 1. ComicInfo ──────────────────────────────────────────────────── */

static void
check_table (GHashTable *meta)
{
  g_assert_nonnull (meta);
  g_assert_cmpstr (g_hash_table_lookup (meta, "title"), ==,
                   "The Test Issue");
  g_assert_cmpstr (g_hash_table_lookup (meta, "series"), ==,
                   "Regression City");
  g_assert_cmpstr (g_hash_table_lookup (meta, "number"), ==, "20");
  g_assert_cmpstr (g_hash_table_lookup (meta, "volume"), ==, "2026");
  g_assert_cmpstr (g_hash_table_lookup (meta, "author"), ==, "Ada Author");
  g_assert_cmpstr (g_hash_table_lookup (meta, "penciller"), ==,
                   "Pablo Penciller");
  g_assert_cmpstr (g_hash_table_lookup (meta, "publisher"), ==,
                   "Sweep Comics");
  g_assert_cmpstr (g_hash_table_lookup (meta, "genre"), ==, "Testing");
}

static void
test_comicinfo (const char *dir)
{
  g_test_message ("comicinfo: parse + archive extraction");

  /* In-memory parse. */
  GHashTable *parsed = fw_comicinfo_parse (COMICINFO_XML,
                                           strlen (COMICINFO_XML));
  check_table (parsed);
  g_hash_table_unref (parsed);

  /* Garbage and empty inputs must not crash or produce tables. */
  g_assert_null (fw_comicinfo_parse ("not xml at all", 13));
  g_assert_null (fw_comicinfo_parse (NULL, 0));
  const char *wrong_root = "<?xml version=\"1.0\"?><NotComicInfo/>";
  g_assert_null (fw_comicinfo_parse (wrong_root, strlen (wrong_root)));

  /* Archive walk (ZIP via the same path CBZ uses). */
  char *cbz = write_comic_zip (dir, "regress.cbz", TRUE);
  GHashTable *meta = fw_comicinfo_extract_from_path (cbz);
  check_table (meta);
  g_hash_table_unref (meta);

  /* No sidecar → NULL, and no crash. */
  char *plain = write_comic_zip (dir, "noinfo.cbz", FALSE);
  g_assert_null (fw_comicinfo_extract_from_path (plain));
  g_assert_null (fw_comicinfo_extract_from_path ("/nonexistent.cbz"));

  /* CBZ through the MuPDF backend: fz knows nothing here, so the
   * merged table is the ComicInfo one and the format row is absent. */
  GError *error = NULL;
  FwDocument *cbz_doc = fw_document_new_for_path (cbz, &error);
  g_assert_no_error (error);
  g_assert_nonnull (cbz_doc);
  GHashTable *cbz_meta = fw_document_get_metadata (cbz_doc);
  check_table (cbz_meta);
  g_hash_table_unref (cbz_meta);
  g_object_unref (cbz_doc);

  g_free (cbz);
  g_free (plain);
}

static void
test_cbr_metadata (const char *dir)
{
  g_test_message ("comicinfo: CBR backend get_metadata");

  /* A .cbr path routes to the libarchive backend even though this
   * fixture is a zip — the backend's reader accepts both. */
  char *cbr = write_comic_zip (dir, "regress.cbr", TRUE);
  GError *error = NULL;
  FwDocument *doc = fw_document_new_for_path (cbr, &error);
  g_assert_no_error (error);
  g_assert_nonnull (doc);
  g_assert_cmpint (fw_document_get_page_count (doc), ==, 2);

  GHashTable *meta = fw_document_get_metadata (doc);
  check_table (meta);
  g_assert_cmpstr (g_hash_table_lookup (meta, "format"), ==,
                   "Comic (RAR)");
  g_hash_table_unref (meta);
  g_object_unref (doc);

  /* Without a sidecar: metadata stays NULL (interface contract). */
  char *plain = write_comic_zip (dir, "noinfo.cbr", FALSE);
  FwDocument *doc2 = fw_document_new_for_path (plain, &error);
  g_assert_no_error (error);
  g_assert_null (fw_document_get_metadata (doc2));
  g_object_unref (doc2);

  g_free (cbr);
  g_free (plain);
}

/* ── 2. CBR cancellation must not be sticky ────────────────────────── */

static void
test_cbr_cancel_not_sticky (const char *dir)
{
  g_test_message ("cbr: cancel_render must not kill future renders");

  char *cbr = write_comic_zip (dir, "cancel.cbr", FALSE);
  GError *error = NULL;
  FwDocument *doc = fw_document_new_for_path (cbr, &error);
  g_assert_no_error (error);
  g_assert_nonnull (doc);

  /* Baseline render, then a scrub-style cancel, then render again.
   * The Phase 20 bug: the second render returned NULL forever. */
  cairo_surface_t *first = fw_document_render_page (doc, 0, 1.0, 0);
  g_assert_nonnull (first);
  cairo_surface_destroy (first);

  fw_document_cancel_render (doc);

  cairo_surface_t *after = fw_document_render_page (doc, 0, 1.0, 0);
  g_assert_nonnull (after);
  cairo_surface_destroy (after);

  /* And one through the handle path the cache actually uses. */
  gpointer handle = fw_document_open_page (doc, 1);
  g_assert_nonnull (handle);
  cairo_surface_t *via_handle =
    fw_document_render_page_from_handle (doc, handle, 1.0, 0);
  g_assert_nonnull (via_handle);
  cairo_surface_destroy (via_handle);
  fw_document_close_page (doc, handle);

  g_object_unref (doc);
  g_free (cbr);
}

/* ── 3. Search indicator clears when the scan finishes ─────────────── */

typedef struct {
  gboolean finished;
} SearchSpy;

static void
on_search_finished (FwSearch *s G_GNUC_UNUSED, gpointer user_data)
{
  SearchSpy *spy = user_data;
  spy->finished = TRUE;
}

static void
test_search_indicator_clears (const char *dir)
{
  g_test_message ("search: is_running clears after a natural finish");

  char *cbz = write_comic_zip (dir, "search.cbz", FALSE);
  GError *error = NULL;
  FwDocument *doc = fw_document_new_for_path (cbz, &error);
  g_assert_no_error (error);
  g_assert_nonnull (doc);

  FwSearch *search = fw_search_new ();
  fw_search_set_document (search, doc);

  SearchSpy spy = { .finished = FALSE };
  g_signal_connect (search, "search-finished",
                    G_CALLBACK (on_search_finished), &spy);

  fw_search_find (search, "zz-nothing", 0);
  g_assert_true (fw_search_is_running (search));

  /* Pump the main loop until the finished message lands (it is posted
   * via g_idle_add from the worker) with a wall-clock guard. */
  GTimer *timer = g_timer_new ();
  while (!spy.finished && g_timer_elapsed (timer, NULL) < 10.0)
    g_main_context_iteration (NULL, TRUE);
  g_timer_destroy (timer);

  g_assert_true (spy.finished);
  /* The Phase 20 bug: this stayed TRUE forever after a natural finish. */
  g_assert_false (fw_search_is_running (search));

  g_object_unref (search);
  g_object_unref (doc);
  g_free (cbz);
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
  fixture.dir = g_dir_make_tmp ("fw-regress-XXXXXX", &error);
  g_assert_no_error (error);
}

static void
teardown (void)
{
  const char *files[] = { "regress.cbz", "noinfo.cbz", "regress.cbr",
                          "noinfo.cbr", "cancel.cbr", "search.cbz" };
  for (size_t i = 0; i < G_N_ELEMENTS (files); i++) {
    char *path = g_build_filename (fixture.dir, files[i], NULL);
    g_remove (path);
    g_free (path);
  }
  g_rmdir (fixture.dir);
}

static void
test_comicinfo_wrap (void)      { test_comicinfo (fixture.dir); }
static void
test_cbr_metadata_wrap (void)   { test_cbr_metadata (fixture.dir); }
static void
test_cbr_cancel_wrap (void)     { test_cbr_cancel_not_sticky (fixture.dir); }
static void
test_search_wrap (void)         { test_search_indicator_clears (fixture.dir); }

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  setup ();
  g_test_add_func ("/phase20/comicinfo", test_comicinfo_wrap);
  g_test_add_func ("/phase20/cbr-metadata", test_cbr_metadata_wrap);
  g_test_add_func ("/phase20/cbr-cancel-not-sticky", test_cbr_cancel_wrap);
  g_test_add_func ("/phase20/search-indicator", test_search_wrap);
  int status = g_test_run ();
  teardown ();
  return status;
}
