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
  /* Reflow-only: first-block index of the active page when the doc
   * was last closed. -1 = not a reflow doc (or never saved). The
   * fixed-layout pipeline ignores this field; the reflow pipeline
   * ignores zoom_level / zoom_mode / view_mode / rotation. */
  int    reflow_block;
} FwDocumentState;

void             fw_state_init    (void);
FwDocumentState *fw_state_load    (const char *path);
void             fw_state_save    (const char            *path,
                                   const FwDocumentState *state);
void             fw_state_prune   (void);

void             fw_document_state_free (FwDocumentState *state);

G_END_DECLS
