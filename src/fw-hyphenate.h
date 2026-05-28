/* fw-hyphenate.h — Knuth-Liang hyphenation (en_US only, bundled).
 *
 * Loads TeX en_US hyphenation patterns vendored under
 * data/hyphenation/ at first call. All entry points are thread-safe
 * after init; init itself is g_once_init-guarded.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FW_HYPHENATE_H
#define FW_HYPHENATE_H

#include <glib.h>

G_BEGIN_DECLS

/* True when Framework can hyphenate documents in the given BCP-47-ish
 * language tag (e.g. "en", "en-US", "en_GB", NULL). Used by the reflow
 * pipeline to skip hyphenation when the document declares a language
 * we don't have patterns for. NULL / empty is treated as English (the
 * "unknown" gate). */
gboolean fw_hyphenate_supports_lang (const char *lang);

/* Find hyphen positions for a single word. `word` is UTF-8; only the
 * ASCII a-z range participates in matching (callers should lowercase
 * before calling — that's how the patterns are written). `len` is the
 * byte length of the word. `leftmin` / `rightmin` are TeX's hyphenmin
 * parameters: never split closer than leftmin chars to the start or
 * rightmin chars to the end. `out_positions` receives byte offsets
 * within `word` where a soft hyphen may be inserted (i.e. before
 * word[off]). Returns the number of positions written, capped at
 * `max_positions`. Returns 0 if the word is too short, contains
 * unsupported characters, or has no valid breaks. */
int fw_hyphenate_word (const char *word, int len,
                       int leftmin, int rightmin,
                       int *out_positions, int max_positions);

G_END_DECLS

#endif /* FW_HYPHENATE_H */
