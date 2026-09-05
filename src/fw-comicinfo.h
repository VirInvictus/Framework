/* fw-comicinfo.h — ComicInfo.xml metadata extraction
 *
 * Reads the de-facto-standard `ComicInfo.xml` sidecar that comic
 * archives carry alongside their page images and turns it into the
 * same string-keyed GHashTable the FwDocument::get_metadata interface
 * returns (keys "title", "author", "series", …; values newly allocated
 * strings). Both comic paths share it: the libarchive CBR backend
 * parses during its open-time enumeration walk (a RAR stream must be
 * decompressed front-to-back, so a second pass at dialog time would
 * stall the UI), and the MuPDF backend's CBZ/CB7/CBT path walks the
 * archive lazily (ZIP/7z/tar have directories; a metadata-only pass is
 * cheap).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* TRUE when `name` is a ComicInfo.xml sidecar (any directory,
 * case-insensitive). Shared with the CBR backend so its open-time
 * enumeration walk can spot the entry without a second pass. */
gboolean fw_comicinfo_name_matches (const char *name);

/* Walk the archive at `path` looking for a `ComicInfo.xml` entry (any
 * directory, case-insensitive), parse it, and return the metadata
 * table. Returns NULL when the archive has no ComicInfo.xml, it cannot
 * be read, or it does not parse. Hash keys/values are g_free'd. */
GHashTable *fw_comicinfo_extract_from_path (const char *path);

/* Parse an in-memory ComicInfo.xml document. Same table contract as
 * above; NULL on parse failure or when no recognized fields are
 * present. */
GHashTable *fw_comicinfo_parse (const char *xml, gsize len);

G_END_DECLS
