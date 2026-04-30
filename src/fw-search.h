/* fw-search.h — Async search controller
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "fw-document.h"

G_BEGIN_DECLS

#define FW_TYPE_SEARCH (fw_search_get_type ())

G_DECLARE_FINAL_TYPE (FwSearch, fw_search, FW, SEARCH, GObject)

FwSearch *fw_search_new          (void);
void      fw_search_set_document (FwSearch   *self,
                                  FwDocument *document);

/* Begin a new search. Cancels any in-flight scan, clears previous hits,
 * and starts a background worker that scans page-by-page beginning at
 * start_page (typically the user's current page so they hit nearby
 * matches first). Emits "hits-changed" each time a batch of new hits is
 * appended, "current-changed" when the active hit moves, and
 * "search-finished" once every page has been scanned. */
void      fw_search_find         (FwSearch   *self,
                                  const char *text,
                                  int         start_page);

/* Cancel any in-flight scan and clear results. */
void      fw_search_clear        (FwSearch   *self);

void      fw_search_next         (FwSearch   *self);
void      fw_search_prev         (FwSearch   *self);

int       fw_search_get_count    (FwSearch   *self);
int       fw_search_get_current  (FwSearch   *self);
gboolean  fw_search_is_running   (FwSearch   *self);

/* Returns the active hit's page, or -1 if none. */
int       fw_search_get_current_page (FwSearch *self);

/* Returns a newly allocated GArray (FwSearchHit) of all hits on `page`,
 * including the active-hit index within that array via *out_active_idx
 * (or -1 if the active hit is on a different page). Caller frees with
 * g_array_unref. NULL if no hits on this page. */
GArray   *fw_search_hits_for_page (FwSearch *self,
                                   int       page,
                                   int      *out_active_idx);

/* Direct access to all hits as a contiguous FwSearchHit array — used by the
 * view's snapshot loop to avoid per-page array copies. The returned pointer
 * is owned by FwSearch and is valid until the next find/clear/dispose. */
const FwSearchHit *fw_search_peek_hits (FwSearch *self, int *out_count);

/* The hit at this index is currently active (highlighted with a stronger
 * colour). Returns -1 if there is no active hit. */
int       fw_search_active_index (FwSearch *self);

G_END_DECLS
