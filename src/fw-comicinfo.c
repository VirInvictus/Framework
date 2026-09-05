/* fw-comicinfo.c — ComicInfo.xml metadata extraction
 *
 * See fw-comicinfo.h. The XML side is a small libxml2 tree walk over
 * the standard field set; the archive side is one libarchive header
 * pass (data decompressed only for the ComicInfo.xml entry itself).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-comicinfo.h"

#include <archive.h>
#include <archive_entry.h>
#include <libxml/parser.h>
#include <string.h>

/* ComicInfo.xml is small by convention (a few KB); refuse anything
 * absurd so a hostile archive can't make us buffer gigabytes. */
#define COMICINFO_MAX_BYTES (1u * 1024u * 1024u)

gboolean
fw_comicinfo_name_matches (const char *name)
{
  if (!name)
    return FALSE;
  const char *base = strrchr (name, '/');
  base = base ? base + 1 : name;
  return g_ascii_strcasecmp (base, "ComicInfo.xml") == 0;
}

/* Read the entry the reader is parked on into a fresh buffer. Returns
 * NULL when the entry is missing, oversized, or truncates. */
static guint8 *
read_current_entry (struct archive *a, struct archive_entry *entry, gsize *out_len)
{
  gint64 sz = archive_entry_size (entry);
  if (sz <= 0 || sz > COMICINFO_MAX_BYTES)
    return NULL;

  guint8 *bytes = g_malloc ((size_t) sz);
  size_t total = 0;
  while (total < (size_t) sz) {
    la_ssize_t got = archive_read_data (a, bytes + total,
                                        (size_t) sz - total);
    if (got <= 0) {
      g_free (bytes);
      return NULL;
    }
    total += (size_t) got;
  }
  *out_len = total;
  return bytes;
}

/* NUL-terminate and trim an element's text content; "" collapses to a
 * skip at the caller. */
static char *
trimmed_content (xmlNode *node)
{
  xmlChar *raw = xmlNodeGetContent (node);
  if (!raw)
    return NULL;
  char *trimmed = g_strdup ((const char *) raw);
  xmlFree (raw);
  g_strstrip (trimmed);
  return trimmed;
}

static void
store_if_set (GHashTable *out, const char *key, const char *value)
{
  if (value && *value)
    g_hash_table_replace (out, g_strdup (key), g_strdup (value));
}

GHashTable *
fw_comicinfo_parse (const char *xml, gsize len)
{
  if (!xml || len == 0)
    return NULL;

  xmlDoc *doc = xmlReadMemory (xml, (int) len, "ComicInfo.xml", NULL,
                               XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
                               XML_PARSE_NONET);
  if (!doc)
    return NULL;

  xmlNode *root = xmlDocGetRootElement (doc);
  GHashTable *out = NULL;
  if (root && root->name &&
      g_ascii_strcasecmp ((const char *) root->name, "ComicInfo") == 0) {
    out = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    for (xmlNode *n = root->children; n; n = n->next) {
      if (n->type != XML_ELEMENT_NODE)
        continue;
      char *text = trimmed_content (n);
      if (!text || !*text) {
        g_free (text);
        continue;
      }
      /* Only the fields Framework's properties dialog surfaces; unknown
       * elements are ignored so schema extensions don't warn. */
      if (!g_ascii_strcasecmp ((const char *) n->name, "Title"))
        store_if_set (out, "title", text);
      else if (!g_ascii_strcasecmp ((const char *) n->name, "Series"))
        store_if_set (out, "series", text);
      else if (!g_ascii_strcasecmp ((const char *) n->name, "Number"))
        store_if_set (out, "number", text);
      else if (!g_ascii_strcasecmp ((const char *) n->name, "Volume"))
        store_if_set (out, "volume", text);
      else if (!g_ascii_strcasecmp ((const char *) n->name, "Writer"))
        store_if_set (out, "author", text);
      else if (!g_ascii_strcasecmp ((const char *) n->name, "Penciller"))
        store_if_set (out, "penciller", text);
      else if (!g_ascii_strcasecmp ((const char *) n->name, "Publisher"))
        store_if_set (out, "publisher", text);
      else if (!g_ascii_strcasecmp ((const char *) n->name, "Genre"))
        store_if_set (out, "genre", text);
      g_free (text);
    }
    if (g_hash_table_size (out) == 0) {
      g_hash_table_unref (out);
      out = NULL;
    }
  }

  xmlFreeDoc (doc);
  return out;
}

GHashTable *
fw_comicinfo_extract_from_path (const char *path)
{
  if (!path)
    return NULL;

  struct archive *a = archive_read_new ();
  if (!a)
    return NULL;
  archive_read_support_format_rar  (a);
  archive_read_support_format_rar5 (a);
  archive_read_support_format_tar  (a);
  archive_read_support_format_7zip (a);
  archive_read_support_format_zip  (a);
  if (archive_read_open_filename (a, path, 64 * 1024) != ARCHIVE_OK) {
    archive_read_free (a);
    return NULL;
  }

  GHashTable *out = NULL;
  struct archive_entry *entry;
  while (archive_read_next_header (a, &entry) == ARCHIVE_OK) {
    if (!fw_comicinfo_name_matches (archive_entry_pathname (entry))) {
      archive_read_data_skip (a);
      continue;
    }
    gsize len = 0;
    guint8 *bytes = read_current_entry (a, entry, &len);
    if (bytes) {
      out = fw_comicinfo_parse ((const char *) bytes, len);
      g_free (bytes);
    }
    break;   /* first ComicInfo.xml wins, whatever it held */
  }

  archive_read_free (a);
  return out;
}
