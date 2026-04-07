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
} FwDocumentState;

void             fw_state_init    (void);
FwDocumentState *fw_state_load    (const char *path);
void             fw_state_save    (const char            *path,
                                   const FwDocumentState *state);
void             fw_state_prune   (void);

void             fw_document_state_free (FwDocumentState *state);

G_END_DECLS
