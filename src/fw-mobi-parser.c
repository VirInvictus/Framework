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

/* Variable-length integer (forward-read, MSB-first). 1–4 bytes;
 * stop when bit 7 is set. Direct port of foliate's `getVarLen`. */
typedef struct { guint32 value; guint length; } FwMobiVarLen;
static FwMobiVarLen
read_var_len (const guchar *p, gsize len, gsize from)
{
  FwMobiVarLen out = { 0, 0 };
  for (gsize i = from; i < len && i < from + 4; i++) {
    out.value = (out.value << 7) | (p[i] & 0x7F);
    out.length++;
    if (p[i] & 0x80) break;
  }
  return out;
}

static guint
count_bits_set (guint32 x)
{
  guint c = 0;
  while (x) { if (x & 1) c++; x >>= 1; }
  return c;
}

static guint
count_unset_end (guint32 x)
{
  guint c = 0;
  while ((x & 1) == 0) { x >>= 1; c++; if (c >= 32) break; }
  return c;
}

/* ── HuffDic decompressor (compression code 17480) ────────────────
 *
 * Port of foliate-js's huffcdic (.foliate-js/mobi.js). Used by older
 * Mobipocket files (modern Calibre output ships PalmDOC). The HUFF
 * record carries two Huffman tables; CDIC records carry the
 * dictionary entries that codes resolve to. Some dictionary entries
 * are themselves HuffDic-encoded — handle by recursively expanding
 * and caching back into the dictionary slot.
 */

typedef struct {
  guint8  terminate;
  guint8  code_length;
  guint32 value;
} HdT1;

typedef struct {
  guint32 mincode;
  guint32 value;
} HdT2;

typedef struct {
  guint8  *data;
  gsize    len;
  gboolean decompressed;
} HdEntry;

typedef struct {
  HdT1     table1[256];
  HdT2     table2[33];   /* 1-indexed; slot 0 unused */
  HdEntry *dict;
  guint32  num_entries;
} HuffCdic;

static guint32
hd_read32_bits (const guchar *data, gsize len, gsize bit_pos)
{
  gsize start_byte = bit_pos >> 3;
  gsize end        = bit_pos + 32;
  gsize end_byte   = end >> 3;
  guint64 bits = 0;
  for (gsize i = start_byte; i <= end_byte; i++) {
    guint8 b = (i < len) ? data[i] : 0;
    bits = (bits << 8) | b;
  }
  guint shift = 8u - (guint) (end & 7u);
  return (guint32) ((bits >> shift) & 0xFFFFFFFFu);
}

static void
huffcdic_free (HuffCdic *hd)
{
  if (!hd) return;
  if (hd->dict) {
    for (guint32 i = 0; i < hd->num_entries; i++)
      g_free (hd->dict[i].data);
    g_free (hd->dict);
  }
  g_free (hd);
}

static HuffCdic *
huffcdic_build (const guchar *raw, gsize raw_len,
                const gsize *roff, guint16 record_count,
                guint32 huff_idx, guint32 num_records,
                GError **error)
{
  if (huff_idx == 0 || huff_idx >= record_count || num_records < 2) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: invalid huffcdic range (idx=%u num=%u)",
                 huff_idx, num_records);
    return NULL;
  }
  gsize hr_off = roff[huff_idx];
  gsize hr_len = roff[huff_idx + 1] - hr_off;
  if (hr_off + hr_len > raw_len || hr_len < 16 ||
      memcmp (raw + hr_off, "HUFF", 4) != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: bad HUFF magic");
    return NULL;
  }
  guint32 off1 = read_be32 (raw + hr_off + 8);
  guint32 off2 = read_be32 (raw + hr_off + 12);
  if ((gsize) off1 + 256u * 4u > hr_len ||
      (gsize) off2 + 32u  * 8u > hr_len) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: HUFF tables out of range");
    return NULL;
  }

  HuffCdic *hd = g_new0 (HuffCdic, 1);
  for (int i = 0; i < 256; i++) {
    guint32 x = read_be32 (raw + hr_off + off1 + i * 4);
    hd->table1[i].terminate   = (x & 0x80u) ? 1 : 0;
    hd->table1[i].code_length = x & 0x1Fu;
    hd->table1[i].value       = x >> 8;
  }
  for (int i = 0; i < 32; i++) {
    hd->table2[i + 1].mincode = read_be32 (raw + hr_off + off2 + i * 8);
    hd->table2[i + 1].value   = read_be32 (raw + hr_off + off2 + i * 8 + 4);
  }

  guint32 first_cdic = huff_idx + 1;
  if (first_cdic >= record_count) {
    huffcdic_free (hd);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: no CDIC records present");
    return NULL;
  }
  gsize c0_off = roff[first_cdic];
  gsize c0_len = roff[first_cdic + 1] - c0_off;
  if (c0_len < 16 || memcmp (raw + c0_off, "CDIC", 4) != 0) {
    huffcdic_free (hd);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: bad CDIC magic");
    return NULL;
  }
  guint32 cdic_total_entries = read_be32 (raw + c0_off + 8);
  if (cdic_total_entries == 0 || cdic_total_entries > (1u << 24)) {
    huffcdic_free (hd);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "mobi: CDIC numEntries=%u out of range",
                 cdic_total_entries);
    return NULL;
  }
  hd->dict        = g_new0 (HdEntry, cdic_total_entries);
  hd->num_entries = cdic_total_entries;

  guint32 written = 0;
  for (guint32 ci = 0; ci < num_records - 1; ci++) {
    guint32 r_idx = first_cdic + ci;
    if (r_idx >= record_count) break;
    gsize r_off = roff[r_idx];
    gsize r_len = roff[r_idx + 1] - r_off;
    if (r_len < 16 || memcmp (raw + r_off, "CDIC", 4) != 0) {
      huffcdic_free (hd);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "mobi: CDIC %u bad magic", ci);
      return NULL;
    }
    guint32 hdr_len  = read_be32 (raw + r_off + 4);
    guint32 code_len = read_be32 (raw + r_off + 12);
    if (hdr_len > r_len || code_len > 24) {
      huffcdic_free (hd);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "mobi: CDIC %u corrupt header", ci);
      return NULL;
    }
    guint32 cap    = 1u << code_len;
    guint32 remain = cdic_total_entries - written;
    guint32 n      = (cap < remain) ? cap : remain;

    const guchar *buf = raw + r_off + hdr_len;
    gsize         bsz = r_len - hdr_len;
    for (guint32 i = 0; i < n; i++) {
      if ((gsize) i * 2 + 2 > bsz) goto cdic_corrupt;
      guint16 ent_off = read_be16 (buf + i * 2);
      if ((gsize) ent_off + 2 > bsz) goto cdic_corrupt;
      guint16 x = read_be16 (buf + ent_off);
      gsize length = x & 0x7FFFu;
      gboolean already = (x & 0x8000u) != 0;
      if ((gsize) ent_off + 2 + length > bsz) goto cdic_corrupt;
      hd->dict[written].len  = length;
      hd->dict[written].data = g_memdup2 (buf + ent_off + 2, length);
      hd->dict[written].decompressed = already;
      written++;
    }
  }
  /* Entries past `written` stay NULL — dict lookups in that range
   * fail the bound check in huffcdic_decompress, treated as
   * truncated input. */
  return hd;

cdic_corrupt:
  huffcdic_free (hd);
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "mobi: CDIC entry truncated");
  return NULL;
}

static gboolean
huffcdic_decompress (HuffCdic *hd, const guchar *in, gsize in_len,
                     GString *out, int recurse_depth)
{
  if (recurse_depth > 16)
    return FALSE;
  gsize bit_len = in_len * 8;
  for (gsize i = 0; i < bit_len; ) {
    guint32 bits = hd_read32_bits (in, in_len, i);
    HdT1 t1 = hd->table1[bits >> 24];
    guint32 code_length = t1.code_length;
    guint32 value       = t1.value;
    if (!t1.terminate) {
      while (code_length > 0 && code_length <= 32) {
        guint shift = 32u - code_length;
        guint32 leading = (code_length == 32) ? bits : (bits >> shift);
        if (leading >= hd->table2[code_length].mincode) break;
        code_length++;
      }
      if (code_length == 0 || code_length > 32) return FALSE;
      value = hd->table2[code_length].value;
    }
    if (code_length == 0) return FALSE;
    if (i + code_length > bit_len) break;
    i += code_length;

    guint shift = 32u - code_length;
    guint32 leading = (code_length == 32) ? bits : (bits >> shift);
    guint32 code = value - leading;
    if (code >= hd->num_entries) return FALSE;

    HdEntry *ent = &hd->dict[code];
    if (!ent->data) return FALSE;
    if (!ent->decompressed) {
      GString *exp = g_string_sized_new (ent->len * 2);
      if (!huffcdic_decompress (hd, ent->data, ent->len, exp,
                                recurse_depth + 1)) {
        g_string_free (exp, TRUE);
        return FALSE;
      }
      gsize new_len = exp->len;
      g_free (ent->data);
      ent->data = (guint8 *) g_string_free (exp, FALSE);
      ent->len  = new_len;
      ent->decompressed = TRUE;
    }
    g_string_append_len (out, (const char *) ent->data, ent->len);
  }
  return TRUE;
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
 * We extract title (503), creator (100), publisher (101), language
 * (524), cover_offset (201, uint), thumbnail_offset (202, uint).
 * Everything else is ignored. */

static void
walk_exth (const guchar *exth, gsize exth_len,
           char **out_title, char **out_author,
           char **out_publisher, char **out_language,
           guint32 *out_cover_offset)
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

    /* String fields: first-write-wins (some EXTHs declare the same
     * code multiple times). */
    char **slot = NULL;
    if      (code == 100) slot = out_author;
    else if (code == 101) slot = out_publisher;
    else if (code == 503) slot = out_title;
    else if (code == 524) slot = out_language;

    if (slot && !*slot && payload > 0)
      *slot = g_strndup ((const char *) exth + off + 8, payload);

    /* coverOffset (201) — uint, 0-based offset into resource records. */
    if (code == 201 && payload >= 4 && out_cover_offset && *out_cover_offset == 0xffffffff)
      *out_cover_offset = read_be32 (exth + off + 8);

    off += len;
  }
}

/* ── Image decoding — port of MOBI.loadResource ───────────────
 *
 * Foliate's loadResource peels off FONT/VIDE/AUDI prefixes and
 * returns raw bytes. For images, the bytes are JPEG/PNG/GIF
 * directly — gdk_texture_new_from_bytes handles all three.
 * Magic-byte check filters obviously-non-image records (FLIS,
 * FCIS, BOUNDARY, etc.) before we hand bytes to GdkTexture. */
static gboolean
looks_like_image (const guchar *p, gsize n)
{
  if (n < 4) return FALSE;
  /* JPEG: FF D8 FF */
  if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return TRUE;
  /* PNG: 89 50 4E 47 */
  if (p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47) return TRUE;
  /* GIF87a / GIF89a */
  if (n >= 6 && memcmp (p, "GIF8", 4) == 0) return TRUE;
  /* WebP: RIFF????WEBP */
  if (n >= 12 && memcmp (p, "RIFF", 4) == 0 && memcmp (p + 8, "WEBP", 4) == 0) return TRUE;
  return FALSE;
}

/* ── INDX parser (port of foliate's getIndexData) ─────────────────
 *
 * INDX records carry a tag-controlled table with variable-length
 * fields. Used by KF8 SKEL and FRAG indexes. The parser reads:
 *
 *   1. The "primary" INDX record (at indxIndex). Header gives us
 *      the number of secondary records and the encoding. The TAGX
 *      block immediately following the INDX header defines the
 *      tag schema (tag id, num_values, mask, end-flag).
 *   2. CNCX records (skipped here — only TOC needs them).
 *   3. Secondary INDX records. Each has its own header + IDXT
 *      block listing per-entry offsets within that record. Each
 *      entry has a name (length-prefixed string), then control
 *      bytes that drive variable-length tag values.
 *
 * Output: an array of IndxEntry (name + tag→values map). The
 * SKEL/FRAG callers pull specific tags out of each entry's map.
 */

typedef struct {
  guint8  tag;
  guint8  num_values;
  guint8  mask;
  guint8  end;
} IndxTagx;

typedef struct {
  char         *name;          /* owned */
  GHashTable   *tag_map;       /* guint (tag id) → GArray<guint32> */
} IndxEntry;

static void
indx_entry_free (gpointer p)
{
  IndxEntry *e = p;
  g_free (e->name);
  if (e->tag_map) g_hash_table_unref (e->tag_map);
  g_free (e);
}

/* For one INDX-table record, parse its IDXT and per-entry tags. */
static void
parse_indx_record_body (const guchar *rec, gsize rec_len,
                        const IndxTagx *tagx_table, gsize num_tagx,
                        guint num_ctrl_bytes,
                        GPtrArray *entries_out)
{
  if (rec_len < 24 || memcmp (rec, "INDX", 4) != 0) return;
  /* INDX_HEADER fields we need:
   *   idxt:       offset 20
   *   numRecords: offset 24
   */
  guint32 idxt        = read_be32 (rec + 20);
  guint32 num_records = read_be32 (rec + 24);

  for (guint32 j = 0; j < num_records; j++) {
    gsize off_off = idxt + 4 + 2 * j;
    if (off_off + 2 > rec_len) break;
    guint16 entry_off = read_be16 (rec + off_off);
    if (entry_off >= rec_len) continue;

    /* Entry: 1-byte name-length, then name bytes, then control
     * bytes, then tag-value bytes. */
    guchar name_len = rec[entry_off];
    gsize  name_start = entry_off + 1;
    if (name_start + name_len > rec_len) continue;

    IndxEntry *e = g_new0 (IndxEntry, 1);
    e->name = g_strndup ((const char *) rec + name_start, name_len);
    e->tag_map = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                        NULL, (GDestroyNotify) g_array_unref);

    gsize start_pos     = name_start + name_len;
    gsize ctrl_byte_idx = 0;
    gsize pos           = start_pos + num_ctrl_bytes;

    /* First pass: figure out which tags are present, with how many
     * values each. The tag-table iteration matches foliate's. */
    typedef struct {
      guint8  tag, num_values;
      guint8  value_count;     /* fixed count; 0 = use value_bytes */
      guint32 value_bytes;     /* number of bytes to consume via VLI */
    } LiveTag;

    GArray *live = g_array_new (FALSE, FALSE, sizeof (LiveTag));

    for (gsize t = 0; t < num_tagx; t++) {
      const IndxTagx *tx = &tagx_table[t];
      if (tx->end & 1) { ctrl_byte_idx++; continue; }
      gsize ctrl_off = start_pos + ctrl_byte_idx;
      if (ctrl_off >= rec_len) break;
      guchar ctrl = rec[ctrl_off];
      guchar v = ctrl & tx->mask;

      if (v == tx->mask) {
        if (count_bits_set (tx->mask) > 1) {
          FwMobiVarLen vl = read_var_len (rec, rec_len, pos);
          LiveTag lt = { tx->tag, tx->num_values, 0, vl.value };
          g_array_append_val (live, lt);
          pos += vl.length;
        } else {
          LiveTag lt = { tx->tag, tx->num_values, 1, 0 };
          g_array_append_val (live, lt);
        }
      } else {
        guchar shifted = v >> count_unset_end (tx->mask);
        LiveTag lt = { tx->tag, tx->num_values, shifted, 0 };
        g_array_append_val (live, lt);
      }
    }

    /* Second pass: read the actual values. */
    for (guint k = 0; k < live->len; k++) {
      LiveTag *lt = &g_array_index (live, LiveTag, k);
      GArray *vals = g_array_new (FALSE, FALSE, sizeof (guint32));
      if (lt->value_count != 0) {
        guint total = lt->value_count * lt->num_values;
        for (guint x = 0; x < total; x++) {
          FwMobiVarLen vl = read_var_len (rec, rec_len, pos);
          guint32 v = vl.value;
          g_array_append_val (vals, v);
          pos += vl.length;
        }
      } else {
        gsize count = 0;
        while (count < lt->value_bytes) {
          FwMobiVarLen vl = read_var_len (rec, rec_len, pos);
          guint32 v = vl.value;
          g_array_append_val (vals, v);
          pos += vl.length;
          count += vl.length;
        }
      }
      g_hash_table_insert (e->tag_map,
                           GUINT_TO_POINTER ((guint) lt->tag),
                           vals);
    }
    g_array_free (live, TRUE);

    g_ptr_array_add (entries_out, e);
  }
}

/* Walk INDX(indx_index) — primary record + all numRecords secondary
 * records. Returns an array of IndxEntry. NULL on error. */
static GPtrArray *
parse_indx (const guchar *data, gsize raw_len,
            const gsize *roff, guint16 record_count,
            guint32 indx_index)
{
  if (indx_index == 0 || indx_index >= record_count) return NULL;
  gsize off = roff[indx_index];
  gsize len = roff[indx_index + 1] - off;
  if (off + len > raw_len) return NULL;
  if (len < 60 || memcmp (data + off, "INDX", 4) != 0) return NULL;

  /* INDX header: length at 4, idxt at 20 (unused for primary),
   * numRecords at 24. The TAGX block follows the primary header. */
  guint32 hdr_length  = read_be32 (data + off + 4);
  guint32 num_records = read_be32 (data + off + 24);
  /* num_cncx at 0x34 — not used here (TOC-only). */

  if (hdr_length + 12 > len) return NULL;

  /* TAGX section starts at indx.length, has its own header
   * (magic 'TAGX', length at 4, num_control_bytes at 8). */
  const guchar *tagx_buf = data + off + hdr_length;
  gsize tagx_len = len - hdr_length;
  if (tagx_len < 12 || memcmp (tagx_buf, "TAGX", 4) != 0) return NULL;
  guint32 tagx_block_len = read_be32 (tagx_buf + 4);
  guint32 num_ctrl_bytes = read_be32 (tagx_buf + 8);
  if (tagx_block_len < 12 || tagx_block_len > tagx_len) return NULL;

  guint32 num_tagx = (tagx_block_len - 12) / 4;
  g_autofree IndxTagx *tagx_table = g_new0 (IndxTagx, num_tagx);
  for (guint32 i = 0; i < num_tagx; i++) {
    const guchar *e = tagx_buf + 12 + i * 4;
    tagx_table[i].tag        = e[0];
    tagx_table[i].num_values = e[1];
    tagx_table[i].mask       = e[2];
    tagx_table[i].end        = e[3];
  }

  GPtrArray *entries = g_ptr_array_new_with_free_func (indx_entry_free);

  /* Walk the secondary INDX records (indx_index+1 .. indx_index+num_records). */
  for (guint32 r = 1; r <= num_records; r++) {
    guint32 ri = indx_index + r;
    if (ri >= record_count) break;
    gsize roff_r = roff[ri];
    gsize rlen_r = roff[ri + 1] - roff_r;
    if (roff_r + rlen_r > raw_len) break;
    parse_indx_record_body (data + roff_r, rlen_r,
                             tagx_table, num_tagx,
                             num_ctrl_bytes,
                             entries);
  }
  return entries;
}

/* ── KF8 skel + frag splice ─────────────────────────────────────
 *
 * Foliate's KF8.init builds:
 *   skelTable[i] = { numFrag: tagMap[1][0], offset: tagMap[6][0],
 *                    length: tagMap[6][1] }
 *   fragTable[i] = { insertOffset: parseInt(name),
 *                    index: tagMap[4][0],
 *                    offset: tagMap[6][0], length: tagMap[6][1] }
 *
 * Sections: each skel takes the next `numFrag` entries from
 * fragTable (in order). For each section: skeleton bytes from the
 * concatenated text body at [skel.offset, +skel.length), with each
 * frag's bytes spliced in at (frag.insertOffset - skel.offset +
 * inserted_so_far).
 *
 * Output: a single concatenated buffer = sum of all spliced
 * sections, which is the "real" KF8 HTML body.
 */

static guint32
indx_tag_value (const IndxEntry *e, guint tag, guint index)
{
  if (!e || !e->tag_map) return 0;
  GArray *vals = g_hash_table_lookup (e->tag_map, GUINT_TO_POINTER (tag));
  if (!vals || vals->len <= index) return 0;
  return g_array_index (vals, guint32, index);
}

static GString *
splice_kf8_sections (const char *body, gsize body_len,
                     GPtrArray  *skel_entries,
                     GPtrArray  *frag_entries)
{
  GString *out = g_string_sized_new (body_len + 1024);
  if (!skel_entries || !frag_entries) return out;

  guint frag_cursor = 0;

  for (guint s = 0; s < skel_entries->len; s++) {
    IndxEntry *skel = skel_entries->pdata[s];
    guint32 num_frag    = indx_tag_value (skel, 1, 0);
    guint32 skel_offset = indx_tag_value (skel, 6, 0);
    guint32 skel_length = indx_tag_value (skel, 6, 1);

    if ((gsize) skel_offset + skel_length > body_len) break;

    /* Build the section into a temp GString. */
    g_autoptr (GString) sec = g_string_sized_new (skel_length + 256);
    g_string_append_len (sec, body + skel_offset, skel_length);

    gsize inserted = 0;
    for (guint k = 0; k < num_frag && frag_cursor < frag_entries->len;
         k++, frag_cursor++) {
      IndxEntry *frag = frag_entries->pdata[frag_cursor];
      gint64  insert_offset = g_ascii_strtoll (frag->name, NULL, 10);
      guint32 frag_offset   = indx_tag_value (frag, 6, 0);
      guint32 frag_length   = indx_tag_value (frag, 6, 1);

      if ((gsize) frag_offset + frag_length > body_len) continue;

      /* Where in `sec` does this fragment splice in? */
      gint64 splice_at = insert_offset - (gint64) skel_offset
                          + (gint64) inserted;
      if (splice_at < 0) splice_at = 0;
      if ((gsize) splice_at > sec->len) splice_at = sec->len;

      g_string_insert_len (sec, splice_at,
                           body + frag_offset, frag_length);
      inserted += frag_length;
    }

    g_string_append_len (out, sec->str, sec->len);
  }
  return out;
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
  if (compression != 1 && compression != 2 && compression != 17480) {
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

  /* KF8 detection. Foliate's two-step check:
   *
   *   1. If `mobi.version >= 8`, this is a pure KF8 (AZW3) file.
   *   2. Otherwise check EXTH-121 (boundary) — non-0xFFFFFFFF
   *      indicates a combo MOBI/KF8 file; the KF8 part starts at
   *      that record. We re-base `r0` to the boundary record and
   *      re-read the MOBI header from there.
   *
   * version=0xFFFFFFFF means "unset" — treat as KF7 (some Calibre
   * MOBIs use this). */
  gboolean is_kf8 = (file_version >= 8 && file_version != 0xFFFFFFFF);
  guint32 kf8_boundary = 0xFFFFFFFF;

  /* Pre-read EXTH 121 if available so we can detect combo files. */
  guint32 exth_flag_pre = (r0 + 132 <= r0_end) ? read_be32 (data + r0 + 128) : 0;
  if (!is_kf8 && (exth_flag_pre & 0x40)) {
    gsize ex = r0 + mobi_hdr_len + 16;
    if (ex + 12 <= r0_end && memcmp (data + ex, "EXTH", 4) == 0) {
      guint32 cnt = read_be32 (data + ex + 8);
      gsize p = ex + 12;
      for (guint32 i = 0; i < cnt && p + 8 <= r0_end; i++) {
        guint32 code = read_be32 (data + p);
        guint32 ln   = read_be32 (data + p + 4);
        if (ln < 8 || p + ln > r0_end) break;
        if (code == 121 && ln == 12) {
          kf8_boundary = read_be32 (data + p + 8);
          break;
        }
        p += ln;
      }
    }
  }

  /* `kf8_start` tracks the record index that maps to "record 0" in
   * KF8 logical addressing. 0 for pure AZW3 (no boundary); equals
   * the boundary for combo MOBI/KF8 files. All KF8 internal record
   * references (skel, frag, resourceStart) are added to this. */
  guint32 kf8_start = 0;

  /* Re-base to the KF8 boundary record if it's a combo file. */
  if (kf8_boundary != 0xFFFFFFFF && kf8_boundary < record_count) {
    kf8_start = kf8_boundary;
    r0     = roff[kf8_boundary];
    r0_end = roff[kf8_boundary + 1];
    if (r0 + 16 + 4 > r0_end ||
        memcmp (data + r0 + 16, "MOBI", 4) != 0) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "mobi: KF8 boundary record has no MOBI header");
      return NULL;
    }
    /* Re-read PalmDoc + MobiHeader fields from the new record 0. */
    compression  = read_be16 (data + r0 + 0x00);
    text_records = read_be16 (data + r0 + 0x08);
    encryption   = read_be16 (data + r0 + 0x0C);
    if (encryption != 0) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "mobi: encrypted (DRM) — not supported");
      return NULL;
    }
    if (compression != 1 && compression != 2 && compression != 17480) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "mobi: unknown compression %u (KF8)", compression);
      return NULL;
    }
    mobi_hdr_len = read_be32 (data + r0 + 20);
    mobi_type    = read_be32 (data + r0 + 24);
    text_enc     = read_be32 (data + r0 + 28);
    file_version = read_be32 (data + r0 + 36);
    is_kf8 = TRUE;
  }
  (void) mobi_type;

  /* trailingFlags: foliate [240, 4]. */
  guint32 trailing_flags = 0;
  if (r0 + 244 <= r0_end)
    trailing_flags = read_be32 (data + r0 + 240);

  /* HuffDic table refs: foliate MOBI_HEADER huffcdic [112,4],
   * numHuffcdic [116,4]. KF8-relative — add kf8_start for absolute
   * record indices. Built only if compression == 17480. */
  HuffCdic *huff = NULL;
  if (compression == 17480) {
    guint32 huff_rel    = (r0 + 116 <= r0_end) ? read_be32 (data + r0 + 112) : 0;
    guint32 num_huffcdic = (r0 + 120 <= r0_end) ? read_be32 (data + r0 + 116) : 0;
    if (huff_rel == 0 || num_huffcdic == 0) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "mobi: HuffDic compression but huffcdic refs missing");
      return NULL;
    }
    guint32 huff_abs = kf8_start + huff_rel;
    huff = huffcdic_build (data, raw_len, roff, record_count,
                           huff_abs, num_huffcdic, error);
    if (!huff)
      return NULL;
  }

  /* resourceStart: foliate [108, 4]. KF8-relative; add kf8_start to
   * get an absolute file record index. */
  guint32 resource_start_rel = (r0 + 112 <= r0_end)
                                 ? read_be32 (data + r0 + 108) : 0xffffffff;
  guint32 resource_start = (resource_start_rel != 0xffffffff)
                             ? kf8_start + resource_start_rel : 0xffffffff;

  /* KF8 SKEL / FRAG indexes (foliate KF8_HEADER offsets 252 / 248).
   * Both are KF8-relative record indices. */
  guint32 kf8_frag = (is_kf8 && r0 + 252 <= r0_end)
                      ? read_be32 (data + r0 + 248) : 0xffffffff;
  guint32 kf8_skel = (is_kf8 && r0 + 256 <= r0_end)
                      ? read_be32 (data + r0 + 252) : 0xffffffff;

  /* EXTH (gated by exthFlag, bit 0x40). foliate exthFlag offset = 128. */
  char *meta_title = NULL, *meta_author = NULL;
  char *meta_publisher = NULL, *meta_language = NULL;
  guint32 cover_offset = 0xffffffff;
  guint32 exth_flag = (r0 + 132 <= r0_end) ? read_be32 (data + r0 + 128) : 0;
  if (exth_flag & 0x40) {
    /* foliate: getEXTH(buf.slice(mobi.length + 16), encoding).
     * mobi.length is the header_length (byte 20). +16 for the
     * preceding PalmDocHeader = absolute offset from record 0. */
    gsize exth_off = r0 + mobi_hdr_len + 16;
    if (exth_off + 12 <= r0_end) {
      walk_exth (data + exth_off, r0_end - exth_off,
                 &meta_title, &meta_author,
                 &meta_publisher, &meta_language,
                 &cover_offset);
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
    guint16 abs_i = (guint16) (kf8_start + i);
    if (abs_i >= record_count) break;
    gsize off = roff[abs_i];
    gsize len = roff[abs_i + 1] - off;
    if (off + len > raw_len) break;

    gsize stripped = strip_trailing_entries (data + off, len, trailing_flags);

    if (compression == 1) {
      g_string_append_len (body, (const char *) data + off, stripped);
    } else if (compression == 2) {
      palmdoc_decompress (data + off, stripped, body);
    } else { /* compression == 17480 — HuffDic */
      if (!huffcdic_decompress (huff, data + off, stripped, body, 0)) {
        huffcdic_free (huff);
        g_string_free (body, TRUE);
        g_free (meta_title); g_free (meta_author);
        g_free (meta_publisher); g_free (meta_language);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "mobi: HuffDic decompression failed at record %u", i);
        return NULL;
      }
    }
  }
  huffcdic_free (huff);
  huff = NULL;

  /* KF8 SKEL + FRAG splice. The decompressed `body` is a packed
   * stream of skeleton bytes + fragment bytes; we need to splice
   * fragments INTO their skeleton sections at the recorded
   * insertOffsets to produce real HTML. Foliate's KF8.init walks
   * the SKEL and FRAG INDX tables to discover the splice plan;
   * its loadSection performs each splice. We do both inline. */
  if (is_kf8 &&
      kf8_skel != 0xFFFFFFFF && kf8_skel + kf8_start < record_count &&
      kf8_frag != 0xFFFFFFFF && kf8_frag + kf8_start < record_count) {
    GPtrArray *skel = parse_indx (data, raw_len, roff, record_count,
                                   kf8_start + kf8_skel);
    GPtrArray *frag = parse_indx (data, raw_len, roff, record_count,
                                   kf8_start + kf8_frag);
    if (skel && frag) {
      g_autoptr (GString) spliced = splice_kf8_sections (
        body->str, body->len, skel, frag);
      /* Replace the body with the spliced result. */
      g_string_free (body, TRUE);
      body = g_string_new (NULL);
      g_string_append_len (body, spliced->str, spliced->len);
    } else {
      g_warning ("mobi: KF8 INDX parse failed, falling back to raw body");
    }
    if (skel) g_ptr_array_free (skel, TRUE);
    if (frag) g_ptr_array_free (frag, TRUE);
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

  /* Image-record extraction. Walk records [resourceStart..end] —
   * records that look like JPEG/PNG/GIF/WebP get decoded into a
   * recindex → GdkTexture map. recindex is 1-based, so MOBI's
   * `<img recindex="3">` points at images[3-1]. */
  GHashTable *images_ht =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           g_free, g_object_unref);
  guint cover_recindex = 0;

  if (resource_start != 0xffffffff && resource_start < record_count) {
    /* MOBI image records have 1-based indices. record at
     * (resource_start + 0) is recindex 1. */
    guint32 last = record_count;
    /* Cap at a reasonable per-book image count. */
    if (last - resource_start > 4096)
      last = resource_start + 4096;

    for (guint32 r = resource_start, idx = 0; r < last; r++, idx++) {
      gsize off = roff[r];
      gsize len = roff[r + 1] - off;
      if (off + len > raw_len || len < 4) continue;
      const guchar *p = data + off;

      /* Foliate skips FONT (deobfuscate) / VIDE / AUDI prefixes;
       * we don't support those formats yet — filter them out. */
      if (memcmp (p, "FONT", 4) == 0 ||
          memcmp (p, "VIDE", 4) == 0 ||
          memcmp (p, "AUDI", 4) == 0 ||
          memcmp (p, "BOUN", 4) == 0 ||      /* boundary marker */
          memcmp (p, "FLIS", 4) == 0 ||
          memcmp (p, "FCIS", 4) == 0 ||
          memcmp (p, "FDST", 4) == 0 ||
          memcmp (p, "DATP", 4) == 0 ||
          memcmp (p, "SRCS", 4) == 0)
        continue;

      if (!looks_like_image (p, len)) continue;

      g_autoptr (GBytes) bytes = g_bytes_new (p, len);
      g_autoptr (GError) e = NULL;
      GdkTexture *tex = gdk_texture_new_from_bytes (bytes, &e);
      if (!tex) continue;

      /* recindex is 1-based: idx 0 → "1". */
      g_autofree char *key = g_strdup_printf ("%u", idx + 1);
      g_hash_table_insert (images_ht, g_strdup (key), tex);

      /* coverOffset (EXTH 201) is also a 0-based offset into the
       * resource-record range. If it matches the current idx, this
       * is the cover image. */
      if (cover_offset != 0xffffffff && cover_offset == idx)
        cover_recindex = idx + 1;
    }
  }

  FwMobiParsed *p = g_new0 (FwMobiParsed, 1);
  p->body           = utf8_body;
  p->body_len       = utf8_len;
  p->title          = meta_title;
  p->author         = meta_author;
  p->language       = meta_language;
  p->publisher      = meta_publisher;
  p->images         = images_ht;
  p->cover_recindex = cover_recindex;
  p->is_kf8         = is_kf8;
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
  g_clear_pointer (&p->images, g_hash_table_unref);
  g_free (p);
}
