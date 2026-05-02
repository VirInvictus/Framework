/* fw-mobi-parser.h — Low-level MOBI / PalmDB parsing
 *
 * C port of `.foliate-js/mobi.js` core. Reads the PalmDB envelope,
 * locates the PalmDocHeader + MobiHeader in record 0, decompresses
 * the PalmDOC LZ77 text records into one concatenated UTF-8 body.
 *
 * Tolerance properties (matched from foliate-js's JS-runtime
 * accidental tolerance):
 *
 *   - LZ77 bad back-references emit a zero byte and continue
 *     (foliate's `output[output.length - distance]` is undefined,
 *     coerced to 0 by Uint8Array).
 *   - Trailing-data byte counts that exceed the record length clamp
 *     to "strip everything"; record becomes empty (foliate's
 *     `array.subarray(0, -length)` clamps when length > array.length).
 *   - Encrypted (DRM) MOBIs surface as a clean error; out of scope.
 *   - HuffDic compression — foliate handles it; this port flags it
 *     as unsupported for now (rare in modern MOBIs from Calibre).
 *   - KF8 / AZW3 records — foliate splits MOBI/MOBI6/KF8 into three
 *     classes; this port handles MOBI/MOBI6 (KF7) only. KF8 is a
 *     follow-up slice (Phase 5).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Algorithm credit: The PalmDOC LZ77 decoder, EXTH walker, and
 * trailer-stripping logic are direct C ports of the corresponding
 * functions in `.foliate-js/mobi.js` by John Factotum, MIT-licensed.
 */

#pragma once

#include <gio/gio.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

typedef struct {
  /* Concatenated decompressed body — UTF-8 (text_encoding=65001) or
   * Windows-1252 converted to UTF-8. NUL-terminated for parser
   * convenience. May be empty if every record's trailers consumed
   * the whole record. */
  char         *body;
  gsize         body_len;

  /* EXTH-extracted metadata. Each may be NULL. Caller frees. */
  char         *title;
  char         *author;
  char         *language;
  char         *publisher;

  /* Image records — decoded textures keyed by their 1-based MOBI
   * recindex. `<img recindex="N">` resolves to images[N-1]. Cover
   * image (when present) is at the EXTH-coverOffset position;
   * `cover_recindex` is its 1-based index into this hash, or 0 if
   * none. Caller takes ownership: g_hash_table_unref. */
  GHashTable   *images;          /* gchar* "1", "2", ... → GdkTexture* */
  guint         cover_recindex;  /* 1-based; 0 = no cover */

  /* True when the file is a KF8/AZW3 container (we don't extract
   * KF8 records yet — caller should fall through to MuPDF). */
  gboolean      is_kf8;
} FwMobiParsed;

/* Parse the file at `path`. On success returns a heap-allocated
 * FwMobiParsed; on failure returns NULL with *error populated.
 * "Failure" only fires for fatal envelope errors (not a MOBI, DRM,
 * etc.). Bad LZ77 / bad trailer counts produce a parsed struct with
 * possibly-shortened body, not an error. */
FwMobiParsed *fw_mobi_parse       (const char *path, GError **error);
void          fw_mobi_parsed_free (FwMobiParsed *p);

G_END_DECLS
