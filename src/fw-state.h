/* fw-state.h — Per-document state persistence
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int    page;
  double scroll_position;
  double zoom_level;
  char  *zoom_mode;
  char  *view_mode;
  int    rotation;
  /* Legacy: the block-model reflow renderer's first-block index.
   * Still written (and round-tripped through state.json) so old state
   * files stay loadable, but nothing reads it since the WebView path
   * replaced that renderer in v0.76; webview_pos below is the live
   * reflow position. -1 = not a reflow doc (or never saved). */
  int    reflow_block;
  /* WebView-reflow only (EPUB, Phase 17): last reading position as a
   * JSON string `{"anchor":"...","scroll_y":N}` produced by
   * fw_webview_get_cached_position. NULL = not a webview doc (or never
   * saved). The fixed-layout path ignores it. */
  char  *webview_pos;
} FwDocumentState;

void             fw_state_init    (void);
FwDocumentState *fw_state_load    (const char *path);
void             fw_state_save    (const char            *path,
                                   const FwDocumentState *state);
void             fw_state_prune   (void);

void             fw_document_state_free (FwDocumentState *state);

G_END_DECLS
