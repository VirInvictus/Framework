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
 *   3. MOBI INDX zero-length varlen (HIGH, v1.0.1): the while-value_bytes
 *      loop breaks instead of spinning on a zero-length varlen.
 *   4. MOBI INDX fixed-count guard (issue #3, v1.0.3): the fixed-count
 *      value loop breaks on a zero-length varlen instead of appending
 *      value_count*num_values zeroes that poison the KF8 TOC builder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-document.h"
#include "fw-mobi-parser.h"
#include "corpus-root.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* ── Fixture byte helpers ──────────────────────────────────────────── */

static void
s_be16 (GString *s, guint v)
{
  g_string_append_c (s, (v >> 8) & 0xFF);
  g_string_append_c (s, v & 0xFF);
}

static void
s_be32 (GString *s, guint32 v)
{
  g_string_append_c (s, (v >> 24) & 0xFF);
  g_string_append_c (s, (v >> 16) & 0xFF);
  g_string_append_c (s, (v >> 8) & 0xFF);
  g_string_append_c (s, v & 0xFF);
}

static void
s_pad (GString *s, gsize n)
{
  for (gsize i = 0; i < n; i++)
    g_string_append_c (s, 0);
}

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

/* ── 3. MOBI INDX: zero-length varlen must not spin the value loop ─── */

/* A minimal pure-KF8 (AZW3) file whose SKEL and FRAG indexes both point
 * at one poisoned INDX pair: the TAGX entry uses a two-bit mask, so the
 * entry's tag reads a variable-length byte count (127) from the last
 * byte of the record. The second pass then tries to consume 127 varlen
 * bytes past the record end, where read_var_len returns length 0 —
 * which used to advance neither pos nor count: an infinite loop
 * appending zeroes. Records: 0 = header, 1 = text, 2 = primary INDX
 * (with TAGX), 3 = secondary INDX (with the poison entry). */
static char *
write_mobi_indx_zero_len (const char *dir, const char *filename)
{
  /* Record 0: PalmDocHeader + MOBI header, 256 bytes so the KF8
   * skel/frag fields at 252/248 are readable. */
  GString *r0 = g_string_new (NULL);
  s_be16 (r0, 1);            /* compression: none */
  s_be16 (r0, 0);            /* unused */
  s_be32 (r0, 9);            /* text length */
  s_be16 (r0, 1);            /* text record count */
  s_be16 (r0, 4096);         /* record size */
  s_be16 (r0, 0);            /* encryption: none */
  s_be16 (r0, 0);            /* unknown — PalmDocHeader ends at 16 */
  g_string_append_len (r0, "MOBI", 4);
  s_be32 (r0, 232);          /* MOBI header length */
  s_be32 (r0, 2);            /* type: book */
  s_be32 (r0, 65001);        /* encoding: UTF-8 */
  s_be32 (r0, 7);            /* unique-id */
  s_be32 (r0, 8);            /* file version ≥ 8 → KF8 */
  s_pad (r0, 68);            /* 40 → 108 */
  s_be32 (r0, 0xFFFFFFFF);   /* resource start: none */
  s_pad (r0, 80);            /* 112 → 192 */
  s_be32 (r0, 0xFFFFFFFF);   /* FDST index: none */
  s_pad (r0, 44);            /* 196 → 240 */
  s_be32 (r0, 0);            /* trailing flags */
  s_be32 (r0, 0xFFFFFFFF);   /* NCX index: none */
  s_be32 (r0, 2);            /* FRAG index → record 2 */
  s_be32 (r0, 2);            /* SKEL index → record 2 */
  g_assert_cmpuint (r0->len, ==, 256);

  GString *r1 = g_string_new ("<p>hi</p>");

  /* Record 2: the primary INDX + TAGX. Header is 28 bytes so the
   * numRecords field at 24 stays intact, TAGX follows, and the record
   * is padded to the 60-byte minimum parse_indx enforces. */
  GString *r2 = g_string_new (NULL);
  g_string_append_len (r2, "INDX", 4);
  s_be32 (r2, 28);           /* header length */
  s_be32 (r2, 2);            /* index type */
  s_be32 (r2, 0);            /* idxt (unused in the primary) */
  s_be32 (r2, 0);            /* 16..20 */
  s_be32 (r2, 0);            /* 20..24 */
  s_be32 (r2, 1);            /* numRecords: one secondary INDX */
  g_string_append_len (r2, "TAGX", 4);
  s_be32 (r2, 20);           /* TAGX block length */
  s_be32 (r2, 1);            /* one control byte per entry */
  const guint8 tagx_a[4] = { 0x01, 0x01, 0x06, 0x00 };
  const guint8 tagx_b[4] = { 0x00, 0x00, 0x00, 0x01 };
  g_string_append_len (r2, (const gchar *) tagx_a, 4);
  g_string_append_len (r2, (const gchar *) tagx_b, 4);
  s_pad (r2, 12);            /* → 60 (parse_indx's minimum) */
  g_assert_cmpuint (r2->len, ==, 60);

  /* Record 3: the secondary INDX. One IDXT entry pointing at the
   * poison record tail: name "A", control byte 0x06 (matches the
   * two-bit mask), varlen 0xFF (= 127), then the record ends. */
  GString *r3 = g_string_new (NULL);
  g_string_append_len (r3, "INDX", 4);
  s_be32 (r3, 28);           /* header length */
  s_be32 (r3, 2);            /* index type */
  s_pad (r3, 4);             /* 12 → 16 */
  s_be32 (r3, 0);            /* 16..20 */
  s_be32 (r3, 28);           /* idxt offset */
  s_be32 (r3, 1);            /* numRecords: one entry */
  g_string_append_len (r3, "IDXT", 4);
  s_be16 (r3, 34);           /* entry offset */
  g_string_append_c (r3, 1); /* name length */
  g_string_append_c (r3, 'A');
  g_string_append_c (r3, 0x06);  /* control byte: tag 1 present via varlen */
  g_string_append_c (r3, (char) 0xFF);  /* varlen: value 127, record ends here */
  g_assert_cmpuint (r3->len, ==, 38);

  /* Assemble: PDB header, record table, then the records in order. */
  gsize rec0_off = 78 + 4 * 8;
  gsize rec1_off = rec0_off + r0->len;
  gsize rec2_off = rec1_off + r1->len;
  gsize rec3_off = rec2_off + r2->len;

  GString *s = g_string_new (NULL);
  s_pad (s, 60);                       /* name + metadata */
  g_string_append_len (s, "BOOK", 4);  /* type at 60 */
  g_string_append_len (s, "MOBI", 4);  /* creator at 64 */
  s_pad (s, 8);                        /* 68 → 76 (seed + next-list) */
  s_be16 (s, 4);                       /* record count */
  struct { gsize off; } recs[4] = {
    { rec0_off }, { rec1_off }, { rec2_off }, { rec3_off }
  };
  for (int i = 0; i < 4; i++) {
    s_be32 (s, (guint32) recs[i].off);
    g_string_append_c (s, 0);          /* attributes */
    g_string_append_c (s, 0);          /* unique id */
    g_string_append_c (s, 0);
    g_string_append_c (s, (gchar) i);
  }
  g_string_append_len (s, r0->str, r0->len);
  g_string_append_len (s, r1->str, r1->len);
  g_string_append_len (s, r2->str, r2->len);
  g_string_append_len (s, r3->str, r3->len);
  g_string_free (r0, TRUE);
  g_string_free (r1, TRUE);
  g_string_free (r2, TRUE);
  g_string_free (r3, TRUE);

  char *path = g_build_filename (dir, filename, NULL);
  GError *error = NULL;
  g_file_set_contents (path, s->str, s->len, &error);
  g_assert_no_error (error);
  g_string_free (s, TRUE);
  return path;
}

static void
test_mobi_indx_zero_len (const char *dir)
{
  g_test_message ("mobi: INDX value loop breaks on a zero-length varlen");

  char *path = write_mobi_indx_zero_len (dir, "indx-loop.azw3");
  GError *error = NULL;
  FwMobiParsed *parsed = fw_mobi_parse (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (parsed);
  g_assert_true (parsed->is_kf8);
  fw_mobi_parsed_free (parsed);
  g_free (path);
}

/* ── 4. MOBI INDX: the fixed-count loop must break on exhaustion ───── */

/* An AZW3 whose NCX index carries two entries: one valid (tag 6 = the
 * [fid, off] KF8 position [0, 0], tag 3 = the CNCX label offset 0) and
 * one poisoned, declaring the same fixed-count tag 6 from a record
 * exhausted at its control byte. read_var_len past the record end
 * returns length 0, so the unfixed loop never advances pos: it appends
 * value_count*num_values zeroes, and the zero-filled array passes the
 * TOC builder's pos->len >= 2 gate with fid=0/off=0. The CNCX table's
 * offset-0 label then turns the poisoned entry into a bogus second TOC
 * entry. With the guard the poisoned entry carries no positions and is
 * dropped, so exactly one TOC entry (the valid one) survives.
 *
 * Records: 0 = header, 1 = text, 2/3 = FRAG index (one valid fragment
 * so frag_offsets[0] exists), 4/5 = SKEL index (one section covering
 * the body), 6/7 = NCX index (valid entry + poison entry), 8 = CNCX
 * string table ("A" at offset 0). */
static char *
write_mobi_indx_fixed_count (const char *dir, const char *filename)
{
  /* Record 0: PalmDocHeader + MOBI header, 256 bytes (KF8 skel/frag
   * fields at 252/248, NCX at 244). SKEL/FRAG/NCX point at records
   * 4/2/6 (KF8-relative; kf8_start is 0 for a standalone AZW3). */
  GString *r0 = g_string_new (NULL);
  s_be16 (r0, 1);            /* compression: none */
  s_be16 (r0, 0);            /* unused */
  s_be32 (r0, 9);            /* text length */
  s_be16 (r0, 1);            /* text record count */
  s_be16 (r0, 4096);         /* record size */
  s_be16 (r0, 0);            /* encryption: none */
  s_be16 (r0, 0);            /* unknown — PalmDocHeader ends at 16 */
  g_string_append_len (r0, "MOBI", 4);
  s_be32 (r0, 232);          /* MOBI header length */
  s_be32 (r0, 2);            /* type: book */
  s_be32 (r0, 65001);        /* encoding: UTF-8 */
  s_be32 (r0, 7);            /* unique-id */
  s_be32 (r0, 8);            /* file version ≥ 8 → KF8 */
  s_pad (r0, 68);            /* 40 → 108 */
  s_be32 (r0, 0xFFFFFFFF);   /* resource start: none */
  s_pad (r0, 80);            /* 112 → 192 */
  s_be32 (r0, 0xFFFFFFFF);   /* FDST index: none */
  s_pad (r0, 44);            /* 196 → 240 */
  s_be32 (r0, 0);            /* trailing flags */
  s_be32 (r0, 6);            /* NCX index → record 6 */
  s_be32 (r0, 2);            /* FRAG index → record 2 */
  s_be32 (r0, 4);            /* SKEL index → record 4 */
  g_assert_cmpuint (r0->len, ==, 256);

  GString *r1 = g_string_new ("<p>hi</p>");

  /* Records 2/3: the FRAG index. Tag 6 carries the two-value [offset,
   * length]; one control byte, one entry whose fragment covers the
   * whole 9-byte body at insert offset 0. Varlen values need the MSB
   * set to terminate: 0x80 = 0, 0x89 = 9. */
  GString *r2 = g_string_new (NULL);
  g_string_append_len (r2, "INDX", 4);
  s_be32 (r2, 28);           /* header length */
  s_be32 (r2, 2);            /* index type */
  s_be32 (r2, 0);            /* idxt (unused in the primary) */
  s_be32 (r2, 0);            /* 16..20 */
  s_be32 (r2, 0);            /* 20..24 */
  s_be32 (r2, 1);            /* numRecords: one secondary INDX */
  g_string_append_len (r2, "TAGX", 4);
  s_be32 (r2, 20);           /* TAGX block length */
  s_be32 (r2, 1);            /* one control byte per entry */
  const guint8 frag_tagx[2][4] = {
    { 0x06, 0x02, 0x01, 0x00 },   /* tag 6: 2 values, single-bit mask */
    { 0x00, 0x00, 0x00, 0x01 },   /* end marker */
  };
  g_string_append_len (r2, (const gchar *) frag_tagx, 8);
  s_pad (r2, 12);            /* → 60 (parse_indx's minimum) */
  g_assert_cmpuint (r2->len, ==, 60);

  GString *r3 = g_string_new (NULL);
  g_string_append_len (r3, "INDX", 4);
  s_be32 (r3, 28);           /* header length */
  s_be32 (r3, 2);            /* index type */
  s_pad (r3, 4);             /* 12 → 16 */
  s_be32 (r3, 0);            /* 16..20 */
  s_be32 (r3, 28);           /* idxt offset */
  s_be32 (r3, 1);            /* numRecords: one entry */
  g_string_append_len (r3, "IDXT", 4);
  s_be16 (r3, 34);           /* entry offset */
  g_string_append_c (r3, 1); /* name length */
  g_string_append_c (r3, '0');
  g_string_append_c (r3, 0x01);  /* control byte: tag 6, fixed count */
  g_string_append_c (r3, (char) 0x80);  /* frag offset 0 */
  g_string_append_c (r3, (char) 0x89);  /* frag length 9 */
  g_assert_cmpuint (r3->len, ==, 39);

  /* Records 4/5: the SKEL index. Tag 1 (numFrag) and tag 6
   * [offset, length] each get their own control byte via end markers;
   * the section covers the whole body so the splice runs. */
  GString *r4 = g_string_new (NULL);
  g_string_append_len (r4, "INDX", 4);
  s_be32 (r4, 28);           /* header length */
  s_be32 (r4, 2);            /* index type */
  s_be32 (r4, 0);            /* idxt (unused in the primary) */
  s_be32 (r4, 0);            /* 16..20 */
  s_be32 (r4, 0);            /* 20..24 */
  s_be32 (r4, 1);            /* numRecords: one secondary INDX */
  g_string_append_len (r4, "TAGX", 4);
  s_be32 (r4, 28);           /* TAGX block length */
  s_be32 (r4, 2);            /* two control bytes per entry */
  const guint8 skel_tagx[4][4] = {
    { 0x01, 0x01, 0x01, 0x00 },   /* tag 1: 1 value, single-bit mask */
    { 0x00, 0x00, 0x00, 0x01 },   /* end marker */
    { 0x06, 0x02, 0x02, 0x00 },   /* tag 6: 2 values, single-bit mask */
    { 0x00, 0x00, 0x00, 0x01 },   /* end marker */
  };
  g_string_append_len (r4, (const gchar *) skel_tagx, 16);
  s_pad (r4, 4);             /* → 60 */
  g_assert_cmpuint (r4->len, ==, 60);

  GString *r5 = g_string_new (NULL);
  g_string_append_len (r5, "INDX", 4);
  s_be32 (r5, 28);           /* header length */
  s_be32 (r5, 2);            /* index type */
  s_pad (r5, 4);             /* 12 → 16 */
  s_be32 (r5, 0);            /* 16..20 */
  s_be32 (r5, 28);           /* idxt offset */
  s_be32 (r5, 1);            /* numRecords: one entry */
  g_string_append_len (r5, "IDXT", 4);
  s_be16 (r5, 34);           /* entry offset */
  g_string_append_c (r5, 3); /* name length */
  g_string_append_len (r5, "sec", 3);
  g_string_append_c (r5, 0x01);  /* control byte 1: tag 1, fixed count */
  g_string_append_c (r5, 0x02);  /* control byte 2: tag 6, fixed count */
  g_string_append_c (r5, (char) 0x81);  /* numFrag = 1 */
  g_string_append_c (r5, (char) 0x80);  /* skel offset 0 */
  g_string_append_c (r5, (char) 0x89);  /* skel length 9 */
  g_assert_cmpuint (r5->len, ==, 43);

  /* Records 6/7: the NCX index. The primary declares tag 6 (fixed 2
   * values) and tag 3 (fixed 1 value, the CNCX offset) with one
   * control byte each; its TAGX ends at 56, so the last end-marker
   * entry's bytes (00 00 00 01) double as num_cncx = 1 at 0x34. The
   * secondary carries the valid entry first, then the poison. */
  GString *r6 = g_string_new (NULL);
  g_string_append_len (r6, "INDX", 4);
  s_be32 (r6, 28);           /* header length */
  s_be32 (r6, 2);            /* index type */
  s_be32 (r6, 0);            /* idxt (unused in the primary) */
  s_be32 (r6, 0);            /* 16..20 */
  s_be32 (r6, 0);            /* 20..24 */
  s_be32 (r6, 1);            /* numRecords: one secondary INDX */
  g_string_append_len (r6, "TAGX", 4);
  s_be32 (r6, 28);           /* TAGX block length */
  s_be32 (r6, 2);            /* two control bytes per entry */
  const guint8 ncx_tagx[4][4] = {
    { 0x06, 0x02, 0x01, 0x00 },   /* tag 6: 2 values, single-bit mask */
    { 0x00, 0x00, 0x00, 0x01 },   /* end marker */
    { 0x03, 0x01, 0x02, 0x00 },   /* tag 3: 1 value, single-bit mask */
    { 0x00, 0x00, 0x00, 0x01 },   /* end marker (bytes at 0x34 double
                                   * as num_cncx = 1) */
  };
  g_string_append_len (r6, (const gchar *) ncx_tagx, 16);
  s_pad (r6, 4);             /* 56 → 60 */
  g_assert_cmpuint (r6->len, ==, 60);

  GString *r7 = g_string_new (NULL);
  g_string_append_len (r7, "INDX", 4);
  s_be32 (r7, 28);           /* header length */
  s_be32 (r7, 2);            /* index type */
  s_pad (r7, 4);             /* 12 → 16 */
  s_be32 (r7, 0);            /* 16..20 */
  s_be32 (r7, 28);           /* idxt offset */
  s_be32 (r7, 2);            /* numRecords: two entries */
  g_string_append_len (r7, "IDXT", 4);
  s_be16 (r7, 36);           /* entry 0 offset (the valid one) */
  s_be16 (r7, 43);           /* entry 1 offset (the poison; entry 0
                              * ends at 43) */
  g_string_append_c (r7, 1); /* name length */
  g_string_append_c (r7, 'y');
  g_string_append_c (r7, 0x01);  /* control byte 1: tag 6, fixed count */
  g_string_append_c (r7, 0x02);  /* control byte 2: tag 3, fixed count */
  g_string_append_c (r7, (char) 0x80);  /* fid 0 */
  g_string_append_c (r7, (char) 0x80);  /* off 0 */
  g_string_append_c (r7, (char) 0x80);  /* CNCX label offset 0 */
  g_string_append_c (r7, 1); /* name length */
  g_string_append_c (r7, 'x');
  g_string_append_c (r7, 0x01);  /* control byte 1: tag 6, fixed count */
  g_string_append_c (r7, 0x02);  /* control byte 2: tag 3 (unreachable:
                                  * the record ends here, so the value
                                  * reads start at rec_len) */
  g_assert_cmpuint (r7->len, ==, 47);

  /* Record 8: the CNCX string table. One label "A" whose table offset
   * is 0 (the key the poisoned entry's absent tag 3 resolves to). */
  GString *r8 = g_string_new (NULL);
  g_string_append_c (r8, (char) 0x81);  /* varlen: label length 1 */
  g_string_append_c (r8, 'A');

  /* Assemble: PDB header, record table, then the records in order. */
  enum { N_RECS = 9 };
  gsize offs[N_RECS];
  offs[0] = 78 + N_RECS * 8;   /* PDB header + one 8-byte entry per record */
  GString *recs[N_RECS] = { r0, r1, r2, r3, r4, r5, r6, r7, r8 };
  for (int i = 1; i < N_RECS; i++)
    offs[i] = offs[i - 1] + recs[i - 1]->len;

  GString *s = g_string_new (NULL);
  s_pad (s, 60);                       /* name + metadata */
  g_string_append_len (s, "BOOK", 4);  /* type at 60 */
  g_string_append_len (s, "MOBI", 4);  /* creator at 64 */
  s_pad (s, 8);                        /* 68 → 76 (seed + next-list) */
  s_be16 (s, N_RECS);                  /* record count */
  for (int i = 0; i < N_RECS; i++) {
    s_be32 (s, (guint32) offs[i]);
    g_string_append_c (s, 0);          /* attributes */
    g_string_append_c (s, 0);          /* unique id */
    g_string_append_c (s, 0);
    g_string_append_c (s, (gchar) i);
  }
  for (int i = 0; i < N_RECS; i++) {
    g_string_append_len (s, recs[i]->str, recs[i]->len);
    g_string_free (recs[i], TRUE);
  }

  char *path = g_build_filename (dir, filename, NULL);
  GError *error = NULL;
  g_file_set_contents (path, s->str, s->len, &error);
  g_assert_no_error (error);
  g_string_free (s, TRUE);
  return path;
}

static void
test_mobi_indx_fixed_count (const char *dir)
{
  g_test_message ("mobi: INDX fixed-count loop breaks on a zero-length varlen");

  char *path = write_mobi_indx_fixed_count (dir, "indx-count.azw3");
  GError *error = NULL;
  FwMobiParsed *parsed = fw_mobi_parse (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (parsed);
  g_assert_true (parsed->is_kf8);
  /* The splice ran (SKEL+FRAG parsed): skeleton 9 bytes + fragment 9
   * bytes inserted at 0. If this fails the fixture, not the guard, is
   * wrong. */
  g_assert_cmpuint (parsed->body_len, ==, 18);
  /* The poison: the second NCX entry's fixed-count tag 6 reads two
   * values from a record exhausted at its control byte. With the
   * guard the entry carries no positions and is dropped, so exactly
   * the valid entry survives. Without it the loop appended two
   * zeroes, passed the len >= 2 gate with fid=0/off=0, and emitted a
   * bogus second TOC entry via the offset-0 CNCX label. The
   * non-NULL, length-1 assertion also pins the NCX chain itself: a
   * parser regression that bailed out of build_kf8_toc entirely would
   * fail here instead of silently passing. */
  g_assert_nonnull (parsed->toc);
  g_assert_cmpuint (parsed->toc->len, ==, 1);
  const FwMobiTocEntry *te = &g_array_index (parsed->toc, FwMobiTocEntry, 0);
  g_assert_cmpstr (te->label, ==, "A");
  g_assert_cmpuint (te->body_offset, ==, 0);
  fw_mobi_parsed_free (parsed);
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
  const char *files[] = { "longinfo.pdf", "shortinfo.pdf", "indx-loop.azw3",
                          "indx-count.azw3" };
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
static void
test_mobi_wrap (void)       { test_mobi_indx_zero_len (fixture.dir); }
static void
test_mobi_count_wrap (void) { test_mobi_indx_fixed_count (fixture.dir); }

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  setup ();
  g_test_add_func ("/final-audit/pdf-metadata-clamp", test_pdf_long_wrap);
  g_test_add_func ("/final-audit/pdf-metadata-roundtrip", test_pdf_short_wrap);
  g_test_add_func ("/final-audit/djvu-locked-queries", test_djvu_wrap);
  g_test_add_func ("/final-audit/mobi-indx-zero-len", test_mobi_wrap);
  g_test_add_func ("/final-audit/mobi-indx-fixed-count", test_mobi_count_wrap);
  int status = g_test_run ();
  teardown ();
  return status;
}
