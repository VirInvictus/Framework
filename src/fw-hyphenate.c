/* fw-hyphenate.c — Knuth-Liang hyphenation
 *
 * en_US-only for now: loads the TeX hyph-en-us.pat.txt patterns and
 * hyph-en-us.hyp.txt exception list vendored under data/hyphenation/,
 * builds a trie of patterns keyed by literal text, and computes
 * soft-hyphen positions for input words.
 *
 * Single language is enough for Phase 16 Pillar 1: the gating logic
 * (fw_hyphenate_supports_lang) accepts NULL/empty and any "en*" tag;
 * documents declaring a non-English language skip the hyphenation
 * pass at the caller. Adding more pattern files later means another
 * trie/hash per language plus a language → trie lookup; the public
 * API doesn't need to change.
 *
 * Patterns are loaded once via g_once_init. After init the trie and
 * exception table are read-only, so fw_hyphenate_word is thread-safe
 * (the reflow pipeline only calls it from the GTK main thread today,
 * but search/pagination could use a worker later).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-hyphenate.h"
#include "fw-config.h"
#include "fw-debug.h"

#include <string.h>

/* a-z = slots 0..25, '.' = slot 26. Patterns and exception literals
 * are guaranteed lowercase ASCII a-z; '.' appears only as a word-edge
 * anchor in pattern text. Any other byte means "not a hyphenatable
 * word" — the caller bails before reaching the trie walk. */
#define ALPHA_SLOTS 27
#define MAX_WORD    96   /* generous; longer words skip hyphenation */

static inline int
slot_of (guchar c)
{
  if (c >= 'a' && c <= 'z') return c - 'a';
  if (c == '.')             return 26;
  return -1;
}

/* ── Trie ─────────────────────────────────────────────────────────────
 * Each node holds a small linked list of children plus, optionally, a
 * weights array (non-NULL means a pattern terminates at this node).
 * weights[k] is the Knuth-Liang weight at the gap before the k-th
 * literal character (so weight_len = literal_len + 1). Hyphen allowed
 * iff weight is odd.
 *
 * Linked-list children keep memory low — most nodes have one or two
 * children, and the 27-slot space is sparse enough that the per-slot
 * compare is faster than the cache miss a 216-byte child table would
 * cost on a typical node. */
typedef struct TrieNode TrieNode;
typedef struct TrieEdge {
  struct TrieEdge *next;
  TrieNode        *child;
  guint8           slot;
} TrieEdge;

struct TrieNode {
  TrieEdge *children;
  guint8   *weights;
  guint8    weight_len;
};

static TrieNode   *g_pattern_root;
static GHashTable *g_exception_table;   /* char* literal → guint8* positions (0xFF-terminated) */
static gboolean    g_init_ok;

static TrieNode *
trie_node_new (void)
{
  return g_new0 (TrieNode, 1);
}

static TrieNode *
trie_descend (TrieNode *node, int slot)
{
  for (TrieEdge *e = node->children; e; e = e->next)
    if (e->slot == slot)
      return e->child;
  return NULL;
}

static TrieNode *
trie_descend_create (TrieNode *node, int slot)
{
  for (TrieEdge *e = node->children; e; e = e->next)
    if (e->slot == slot)
      return e->child;
  TrieEdge *e = g_new0 (TrieEdge, 1);
  e->slot        = (guint8) slot;
  e->child       = trie_node_new ();
  e->next        = node->children;
  node->children = e;
  return e->child;
}

/* Insert one TeX pattern line. Format: alternating literal chars and
 * weight digits, e.g. ".ad4der" → literal ".adder" with weight 4 at
 * the gap before the second 'd'. Returns FALSE if the line contains
 * anything we can't fit (unknown char, oversize, empty literal).  */
static gboolean
trie_insert_pattern (TrieNode *root, const char *line, gsize len)
{
  guint8 literal[MAX_WORD];
  guint8 weights[MAX_WORD + 1];
  int    lit_len    = 0;
  int    cur_weight = 0;
  memset (weights, 0, sizeof weights);

  for (gsize i = 0; i < len; i++) {
    char c = line[i];
    if (c >= '0' && c <= '9') {
      cur_weight = c - '0';
    } else {
      int s = slot_of ((guchar) c);
      if (s < 0 || lit_len >= MAX_WORD)
        return FALSE;
      literal[lit_len] = (guint8) s;
      weights[lit_len] = (guint8) cur_weight;
      cur_weight = 0;
      lit_len++;
    }
  }
  weights[lit_len] = (guint8) cur_weight;   /* trailing weight slot */

  if (lit_len == 0)
    return FALSE;

  TrieNode *node = root;
  for (int i = 0; i < lit_len; i++)
    node = trie_descend_create (node, literal[i]);

  g_free (node->weights);
  node->weights    = g_memdup2 (weights, lit_len + 1);
  node->weight_len = (guint8) (lit_len + 1);
  return TRUE;
}

/* ── File loading ────────────────────────────────────────────────── */

static gboolean
parse_patterns_file (TrieNode *root, const char *contents, gsize len)
{
  const char *p  = contents;
  const char *eo = contents + len;
  int loaded = 0, skipped = 0;
  while (p < eo) {
    const char *nl  = memchr (p, '\n', eo - p);
    const char *eol = nl ? nl : eo;
    while (eol > p && (eol[-1] == '\r' || eol[-1] == ' ' || eol[-1] == '\t'))
      eol--;
    if (eol > p) {
      if (trie_insert_pattern (root, p, eol - p))
        loaded++;
      else
        skipped++;
    }
    p = nl ? nl + 1 : eo;
  }
  FW_TRACE_WINDOW ("hyphen: loaded %d patterns (%d skipped)", loaded, skipped);
  return loaded > 0;
}

static void
parse_exceptions_file (GHashTable *table, const char *contents, gsize len)
{
  const char *p  = contents;
  const char *eo = contents + len;
  int loaded = 0;
  while (p < eo) {
    const char *nl  = memchr (p, '\n', eo - p);
    const char *eol = nl ? nl : eo;
    while (eol > p && (eol[-1] == '\r' || eol[-1] == ' ' || eol[-1] == '\t'))
      eol--;
    if (eol > p) {
      /* Strip hyphens from the line to get the literal word; the
       * positions where hyphens sat become the allowed break points. */
      GString *literal   = g_string_sized_new (eol - p);
      GArray  *positions = g_array_new (FALSE, FALSE, sizeof (guint8));
      gboolean ok        = TRUE;
      for (const char *q = p; q < eol; q++) {
        if (*q == '-') {
          guint8 pos = (guint8) literal->len;
          g_array_append_val (positions, pos);
        } else if (slot_of ((guchar) *q) >= 0 && *q != '.') {
          g_string_append_c (literal, *q);
        } else {
          ok = FALSE;
          break;
        }
      }
      if (ok && literal->len > 0) {
        guint8 *vec = g_new (guint8, positions->len + 1);
        for (guint i = 0; i < positions->len; i++)
          vec[i] = g_array_index (positions, guint8, i);
        vec[positions->len] = 0xFF;       /* sentinel */
        g_hash_table_replace (table, g_string_free (literal, FALSE), vec);
        literal = NULL;
        loaded++;
      }
      if (literal)
        g_string_free (literal, TRUE);
      g_array_free (positions, TRUE);
    }
    p = nl ? nl + 1 : eo;
  }
  FW_TRACE_WINDOW ("hyphen: loaded %d exceptions", loaded);
}

/* Probe the bundled hyphenation directory, in the same priority order
 * fw-fonts.c uses for fonts (env override → $FRAMEWORK_DATADIR →
 * installed datadir → source-root fallback for dev runs). */
static char *
probe_hyphen_dir (void)
{
  const char *env = g_getenv ("FW_HYPHEN_DIR");
  if (env && *env && g_file_test (env, G_FILE_TEST_IS_DIR))
    return g_strdup (env);

  const char *xdg = g_getenv ("FRAMEWORK_DATADIR");
  if (xdg) {
    char *p = g_build_filename (xdg, "framework", "hyphenation", NULL);
    if (g_file_test (p, G_FILE_TEST_IS_DIR)) return p;
    g_free (p);
  }

  {
    char *p = g_build_filename (FW_DATADIR, "framework", "hyphenation", NULL);
    if (g_file_test (p, G_FILE_TEST_IS_DIR)) return p;
    g_free (p);
  }

  {
    char *p = g_build_filename (FW_SOURCE_ROOT, "data", "hyphenation", NULL);
    if (g_file_test (p, G_FILE_TEST_IS_DIR)) return p;
    g_free (p);
  }
  return NULL;
}

static gboolean
load_data (void)
{
  g_autofree char *dir = probe_hyphen_dir ();
  if (!dir) {
    g_warning ("hyphen: no hyphenation data directory found "
               "(checked FW_HYPHEN_DIR, $FRAMEWORK_DATADIR, "
               FW_DATADIR "/framework/hyphenation, "
               FW_SOURCE_ROOT "/data/hyphenation)");
    return FALSE;
  }

  g_autofree char *pat_path = g_build_filename (dir, "hyph-en-us.pat.txt", NULL);
  g_autofree char *hyp_path = g_build_filename (dir, "hyph-en-us.hyp.txt", NULL);

  gchar *pat_data = NULL;
  gsize  pat_len  = 0;
  GError *err = NULL;
  if (!g_file_get_contents (pat_path, &pat_data, &pat_len, &err)) {
    g_warning ("hyphen: cannot read %s: %s", pat_path,
               err ? err->message : "unknown");
    g_clear_error (&err);
    return FALSE;
  }

  g_pattern_root = trie_node_new ();
  gboolean ok = parse_patterns_file (g_pattern_root, pat_data, pat_len);
  g_free (pat_data);
  if (!ok)
    return FALSE;

  g_exception_table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, g_free);
  gchar *hyp_data = NULL;
  gsize  hyp_len  = 0;
  if (g_file_get_contents (hyp_path, &hyp_data, &hyp_len, NULL)) {
    parse_exceptions_file (g_exception_table, hyp_data, hyp_len);
    g_free (hyp_data);
  }
  /* Exception file missing is non-fatal: just no exceptions. */
  return TRUE;
}

static void
ensure_init (void)
{
  static gsize init_once = 0;
  if (g_once_init_enter (&init_once)) {
    g_init_ok = load_data ();
    g_once_init_leave (&init_once, 1);
  }
}

/* ── Public API ──────────────────────────────────────────────────── */

gboolean
fw_hyphenate_supports_lang (const char *lang)
{
  if (!lang || !*lang)
    return TRUE;
  return g_ascii_strncasecmp (lang, "en", 2) == 0
      && (lang[2] == '\0' || lang[2] == '-' || lang[2] == '_');
}

static int
exception_positions (const char *word, int len,
                     int leftmin, int rightmin,
                     int *out, int max)
{
  if (!g_exception_table)
    return -1;
  char buf[MAX_WORD + 1];
  if (len > MAX_WORD)
    return -1;
  memcpy (buf, word, len);
  buf[len] = 0;
  guint8 *vec = g_hash_table_lookup (g_exception_table, buf);
  if (!vec)
    return -1;
  int n = 0;
  for (int i = 0; vec[i] != 0xFF; i++) {
    int pos = vec[i];
    if (pos >= leftmin && pos <= len - rightmin && n < max)
      out[n++] = pos;
  }
  return n;
}

int
fw_hyphenate_word (const char *word, int len,
                   int leftmin, int rightmin,
                   int *out_positions, int max_positions)
{
  if (!word || len <= 0 || !out_positions || max_positions <= 0)
    return 0;
  if (len > MAX_WORD)
    return 0;
  if (leftmin < 1) leftmin = 1;
  if (rightmin < 1) rightmin = 1;
  if (len < leftmin + rightmin)
    return 0;

  /* Bail early on any non-ASCII-letter byte. Covers contractions
   * (apostrophes), URLs, ALL-CAPS, hyphenated compounds — none of
   * those should pick up extra soft hyphens. */
  for (int i = 0; i < len; i++) {
    guchar c = (guchar) word[i];
    if (c < 'a' || c > 'z')
      return 0;
  }

  ensure_init ();
  if (!g_init_ok || !g_pattern_root)
    return 0;

  /* Exception list overrides pattern matching. */
  int ex_n = exception_positions (word, len, leftmin, rightmin,
                                  out_positions, max_positions);
  if (ex_n >= 0)
    return ex_n;

  /* Frame: ".word." for pattern matching. weights[k] is the weight at
   * the gap before framed[k] (so size = flen + 1; index range 0..flen). */
  guint8 framed[MAX_WORD + 2];
  framed[0] = 26;     /* '.' */
  for (int i = 0; i < len; i++)
    framed[i + 1] = (guint8) (word[i] - 'a');
  framed[len + 1] = 26;
  int flen = len + 2;

  guint8 weights[MAX_WORD + 3];
  memset (weights, 0, flen + 1);

  /* Walk the trie from every starting position; each pattern that
   * matches OR-applies (max) its weights into the framed-word weights. */
  for (int i = 0; i < flen; i++) {
    TrieNode *node = g_pattern_root;
    for (int j = i; j < flen; j++) {
      node = trie_descend (node, framed[j]);
      if (!node)
        break;
      if (node->weights) {
        for (int k = 0; k < node->weight_len; k++) {
          if (node->weights[k] > weights[i + k])
            weights[i + k] = node->weights[k];
        }
      }
    }
  }

  /* A break between word[k-1] and word[k] sits at framed gap k+1
   * (framed[0]='.', framed[1]=word[0], ...). Odd weight = allowed. */
  int n = 0;
  for (int k = leftmin; k <= len - rightmin; k++) {
    if ((weights[k + 1] & 1) && n < max_positions)
      out_positions[n++] = k;
  }
  return n;
}
