/* fw-mobi-parser.c
 *
 * Direct C port of `.foliate-js/mobi.js` (John Factotum, MIT). The
 * goal is byte-for-byte equivalence on well-formed input AND
 * matching tolerance on malformed input — JavaScript's array
 * indexing returns `undefined` (coerced to 0 in Uint8Array), and
 * `subarray(0, -length)` clamps when length > array.length. Both
 * are explicit guards in this port.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-mobi-parser.h"

#include <string.h>
#include <stdint.h>

/* ── Big-endian helpers (PalmDB is BE throughout) ─────────────── */

static guint16 read_be16 (const guchar *p) { return (guint16)(p[0] << 8) | p[1]; }
static guint32 read_be32 (const guchar *p) {
  return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
         ((guint32)p[2] <<  8) |  (guint32)p[3];
}

/* ── PalmDOC LZ77 decompressor — port of decompressPalmDOC ─────
 *
 * foliate-js JavaScript:
 *
 *   const decompressPalmDOC = array => {
 *     let output = []
 *     for (let i = 0; i < array.length; i++) {
 *       const byte = array[i]
 *       if (byte === 0) output.push(0)
 *       else if (byte <= 8)
 *         for (const x of array.subarray(i + 1, (i += byte) + 1))
 *           output.push(x)
 *       else if (byte <= 0b0111_1111) output.push(byte)
 *       else if (byte <= 0b1011_1111) {
 *         const bytes = (byte << 8) | array[i++ + 1]
 *         const distance = (bytes & 0b0011_1111_1111_1111) >>> 3
 *         const length = (bytes & 0b111) + 3
 *         for (let j = 0; j < length; j++)
 *           output.push(output[output.length - distance])
 *       }
 *       else output.push(32, byte ^ 0b1000_0000)
 *     }
 *     return Uint8Array.from(output)
 *   }
 *
 * Tolerance: out-of-range back-reference emits 0 (matches JS
 * undefined→0 coercion). Truncated literal run copies what's
 * available and stops. */
static void
palmdoc_decompress (const guchar *in, gsize in_len, GString *out)
{
  for (gsize i = 0; i < in_len; ) {
    guchar byte = in[i++];
    if (byte == 0) {
      g_string_append_c (out, '\0');
    } else if (byte <= 8) {
      gsize n = byte;
      gsize avail = (i + n <= in_len) ? n : (in_len - i);
      g_string_append_len (out, (const char *) in + i, avail);
      i += avail;
    } else if (byte <= 0x7F) {
      g_string_append_c (out, (char) byte);
    } else if (byte <= 0xBF) {
      if (i >= in_len) break;
      guint16 pair = ((guint16) byte << 8) | in[i++];
      guint distance = (pair & 0x3FFF) >> 3;
      guint length   = (pair & 0x07) + 3;
      for (guint k = 0; k < length; k++) {
        gsize back = out->len >= distance ? out->len - distance : 0;
        char c = (out->len >= distance && distance > 0) ? out->str[back] : 0;
        g_string_append_c (out, c);
      }
    } else { /* 0xC0..0xFF */
      g_string_append_c (out, ' ');
      g_string_append_c (out, (char) (byte ^ 0x80));
    }
  }
}

/* ── Trailing-data stripping — port of getVarLenFromEnd + the
 *    removeTrailingEntries closure in MOBI.#setup ──────────────
 *
 * foliate-js:
 *
 *   const getVarLenFromEnd = byteArray => {
 *     let value = 0
 *     for (const byte of byteArray.subarray(-4)) {
 *       if (byte & 0b1000_0000) value = 0
 *       value = (value << 7) | (byte & 0b111_1111)
 *     }
 *     return value
 *   }
 *
 *   for (let i = 0; i < numTrailingEntries; i++) {
 *     const length = getVarLenFromEnd(array)
 *     array = array.subarray(0, -length)
 *   }
 *   if (multibyte) {
 *     const length = (array[array.length - 1] & 0b11) + 1
 *     array = array.subarray(0, -length)
 *   }
 *
 * Subtlety: if `length > array.length`, JavaScript's
 * `array.subarray(0, -length)` clamps to start, producing an
 * empty array. We match that with `if (length >= len) len = 0`.
 */
static gsize
get_var_len_from_end (const guchar *rec, gsize len)
{
  gsize value = 0;
  /* Iterate the last 4 bytes IN ORDER (matches foliate's
   * `byteArray.subarray(-4)` — slice of last four). */
  gsize start = len >= 4 ? len - 4 : 0;
  for (gsize i = start; i < len; i++) {
    guchar byte = rec[i];
    if (byte & 0x80) value = 0;
    value = (value << 7) | (byte & 0x7F);
  }
  return value;
}

static gsize
strip_trailing_entries (const guchar *rec, gsize len,
                        guint32 trailing_flags)
{
  gboolean multibyte = trailing_flags & 1;
  /* count bits set in (flags >>> 1) */
  guint num_entries = 0;
  for (guint32 t = trailing_flags >> 1; t; t >>= 1)
    if (t & 1) num_entries++;

  for (guint i = 0; i < num_entries; i++) {
    if (len == 0) break;
    gsize length = get_var_len_from_end (rec, len);
    /* JS subarray(0, -length) clamps when length > array.length. */
    if (length >= len) { len = 0; break; }
    len -= length;
  }
  if (multibyte && len > 0) {
    gsize length = (rec[len - 1] & 0x03) + 1;
    if (length >= len) len = 0;
    else               len -= length;
  }
  return len;
}

/* ── EXTH metadata — port of getEXTH ──────────────────────────
 *
 * Foliate's EXTH_RECORD_TYPE table maps codes to (name, type, isArray).
 * We extract just the strings we need: title (503), creator (100),
 * publisher (101), language (524). Everything else is ignored for
 * now. */
typedef struct {
  guint32  code;
  const char *key;
} ExthMap;

static const ExthMap EXTH_MAP[] = {
  { 100, "author" },     /* creator */
  { 101, "publisher" },
  { 503, "title" },
  { 524, "language" },
  { 0,   NULL },
};

static void
walk_exth (const guchar *exth, gsize exth_len,
           char **out_title, char **out_author,
           char **out_publisher, char **out_language)
{
  if (exth_len < 12) return;
  if (memcmp (exth, "EXTH", 4) != 0) return;

  guint32 count = read_be32 (exth + 8);
  gsize off = 12;
  for (guint32 i = 0; i < count && off + 8 <= exth_len; i++) {
    guint32 code = read_be32 (exth + off);
    guint32 len  = read_be32 (exth + off + 4);
    if (len < 8 || off + len > exth_len) break;
    gsize payload = len - 8;

    char **slot = NULL;
    for (const ExthMap *m = EXTH_MAP; m->key; m++) {
      if (m->code == code) {
        if      (g_str_equal (m->key, "author"))    slot = out_author;
        else if (g_str_equal (m->key, "publisher")) slot = out_publisher;
        else if (g_str_equal (m->key, "title"))     slot = out_title;
        else if (g_str_equal (m->key, "language"))  slot = out_language;
        break;
      }
    }
    /* First-write-wins — multiple EXTH records with the same code
     * are common (e.g. multiple authors); we keep the first. */
    if (slot && !*slot && payload > 0)
      *slot = g_strndup ((const char *) exth + off + 8, payload);

    off += len;
  }
}

/* ── Top-level open ───────────────────────────────────────────── */

FwMobiParsed *
fw_mobi_parse (const char *path, GError **error)
{
  g_autofree char *raw = NULL;
  gsize raw_len = 0;
  if (!g_file_get_contents (path, &raw, &raw_len, error))
    return NULL;
  const guchar *data = (const guchar *) raw;

  /* PDB header (78 bytes) + magic check at 0x3C/0x40. */
  if (raw_len < 78) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: file too short for PalmDB header");
    return NULL;
  }
  if (memcmp (data + 0x3C, "BOOK", 4) != 0 ||
      memcmp (data + 0x40, "MOBI", 4) != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "mobi: not a BOOK/MOBI PalmDB");
    return NULL;
  }

  guint16 record_count = read_be16 (data + 0x4C);
  if (record_count == 0 || 78 + (gsize) record_count * 8 > raw_len) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: bad record info list");
    return NULL;
  }

  g_autofree gsize *roff = g_new0 (gsize, record_count + 1);
  for (guint16 i = 0; i < record_count; i++)
    roff[i] = read_be32 (data + 78 + i * 8);
  roff[record_count] = raw_len;

  /* PalmDocHeader at start of record 0; MobiHeader at +16. */
  gsize r0     = roff[0];
  gsize r0_end = roff[1];
  if (r0 + 16 > r0_end) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: record 0 too short");
    return NULL;
  }

  guint16 compression  = read_be16 (data + r0 + 0x00);
  guint16 text_records = read_be16 (data + r0 + 0x08);
  guint16 encryption   = read_be16 (data + r0 + 0x0C);

  if (encryption != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "mobi: encrypted (DRM) — not supported");
    return NULL;
  }
  if (compression == 17480) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "mobi: HuffDic compression — not yet supported");
    return NULL;
  }
  if (compression != 1 && compression != 2) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "mobi: unknown compression %u", compression);
    return NULL;
  }

  /* MobiHeader fields. All offsets are record-0-relative (matching
   * foliate-js's MOBI_HEADER table where each [start, len, type]
   * tuple is an offset from the start of record 0's bytes — which
   * already includes the 16-byte PalmDocHeader). The MOBI magic
   * sits at byte 16 of record 0, then header_length at 20, etc.
   *
   * The MOBI specification numbers fields starting from the MOBI
   * magic (offset 0 of MOBI header = byte 16 of record 0). Either
   * convention works as long as we're consistent — here we follow
   * foliate. */
  if (r0 + 16 + 4 > r0_end ||
      memcmp (data + r0 + 16, "MOBI", 4) != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: missing MOBI magic");
    return NULL;
  }
  guint32 mobi_hdr_len = read_be32 (data + r0 + 20);  /* length */
  guint32 mobi_type    = read_be32 (data + r0 + 24);  /* type */
  guint32 text_enc     = read_be32 (data + r0 + 28);  /* encoding */
  guint32 file_version = read_be32 (data + r0 + 36);  /* version */

  /* foliate detects KF8 by `mobi.version >= 8`. Many KF7 files
   * carry version=0xFFFFFFFF ("unset"), which would falsely trip
   * this; treat that sentinel as "unknown — assume KF7". */
  gboolean is_kf8 = (file_version >= 8 && file_version != 0xFFFFFFFF);
  if (is_kf8) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "mobi: KF8/AZW3 container — Phase 5 follow-up");
    return NULL;
  }
  (void) mobi_type;  /* informational */

  /* trailingFlags: foliate [240, 4]. */
  guint32 trailing_flags = 0;
  if (r0 + 244 <= r0_end)
    trailing_flags = read_be32 (data + r0 + 240);

  /* EXTH (gated by exthFlag, bit 0x40). foliate exthFlag offset = 128. */
  char *meta_title = NULL, *meta_author = NULL;
  char *meta_publisher = NULL, *meta_language = NULL;
  guint32 exth_flag = (r0 + 132 <= r0_end) ? read_be32 (data + r0 + 128) : 0;
  if (exth_flag & 0x40) {
    /* foliate: getEXTH(buf.slice(mobi.length + 16), encoding).
     * mobi.length is the header_length (byte 20). +16 for the
     * preceding PalmDocHeader = absolute offset from record 0. */
    gsize exth_off = r0 + mobi_hdr_len + 16;
    if (exth_off + 12 <= r0_end) {
      walk_exth (data + exth_off, r0_end - exth_off,
                 &meta_title, &meta_author,
                 &meta_publisher, &meta_language);
    }
  }
  /* Title fallback: MobiHeader.titleOffset/Length. foliate offsets
   * 84 and 88 (record-0-relative). */
  if (!meta_title && r0 + 92 <= r0_end) {
    guint32 t_off = read_be32 (data + r0 + 84);
    guint32 t_len = read_be32 (data + r0 + 88);
    /* titleOffset is record-0-relative in foliate's getStruct. */
    gsize abs_off = r0 + t_off;
    if (t_len > 0 && t_len < 1024 && abs_off + t_len <= r0_end)
      meta_title = g_strndup ((const char *) data + abs_off, t_len);
  }

  /* Decompress text records 1..text_records, applying
   * trailing-data stripping before LZ77 (matches foliate's
   * loadText: removeTrailingEntries → decompress). */
  if (text_records == 0 || (gsize) text_records >= record_count) {
    g_free (meta_title); g_free (meta_author);
    g_free (meta_publisher); g_free (meta_language);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: bogus text_records=%u (total=%u)",
                 text_records, record_count);
    return NULL;
  }

  GString *body = g_string_sized_new (text_records * 4096);
  for (guint16 i = 1; i <= text_records; i++) {
    gsize off = roff[i];
    gsize len = roff[i + 1] - off;
    if (off + len > raw_len) break;

    gsize stripped = strip_trailing_entries (data + off, len, trailing_flags);

    if (compression == 1) {
      g_string_append_len (body, (const char *) data + off, stripped);
    } else { /* compression == 2 */
      palmdoc_decompress (data + off, stripped, body);
    }
  }

  /* Convert encoding to UTF-8. */
  char *utf8_body = NULL;
  gsize utf8_len = 0;
  if (text_enc == 1252) {
    g_autoptr (GError) e = NULL;
    utf8_body = g_convert (body->str, body->len, "UTF-8", "WINDOWS-1252",
                           NULL, &utf8_len, &e);
    g_string_free (body, TRUE);
    if (!utf8_body) {
      /* Fallback — keep the raw bytes; the caller's HTML walker
       * will likely reject them, but better than failing the open. */
      utf8_body = g_strdup ("");
      utf8_len = 0;
    }
  } else {
    utf8_len  = body->len;
    utf8_body = g_string_free (body, FALSE);
  }

  FwMobiParsed *p = g_new0 (FwMobiParsed, 1);
  p->body      = utf8_body;
  p->body_len  = utf8_len;
  p->title     = meta_title;
  p->author    = meta_author;
  p->language  = meta_language;
  p->publisher = meta_publisher;
  p->is_kf8    = is_kf8;
  return p;
}

void
fw_mobi_parsed_free (FwMobiParsed *p)
{
  if (!p) return;
  g_free (p->body);
  g_free (p->title);
  g_free (p->author);
  g_free (p->language);
  g_free (p->publisher);
  g_free (p);
}
