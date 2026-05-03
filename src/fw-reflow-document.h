/* fw-reflow-document.h — Reflow document interface (Phase 13.1 Phase 1)
 *
 * Parallel to FwDocument but for formats that flow as a sequence of
 * structurally-typed blocks rather than fixed-layout pages. See
 * docs/foliate-rewrite.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

/* ── Block kinds ──────────────────────────────────────────────────── */

typedef enum {
  FW_BLOCK_HEADING,     /* level encoded in `level` */
  FW_BLOCK_PARAGRAPH,
  FW_BLOCK_BLOCKQUOTE,
  FW_BLOCK_LIST,        /* nested children — not used by TXT */
  FW_BLOCK_LIST_ITEM,
  FW_BLOCK_CODE,        /* preformatted; preserve whitespace */
  FW_BLOCK_IMAGE,
  FW_BLOCK_HR,
  FW_BLOCK_CHAPTER,     /* ToC anchor — no rendered content */
} FwBlockKind;

/* FwBlock flags — packed into the `flags` GUINT. */
#define FW_BLOCK_FLAG_COVER   (1u << 0)  /* IMAGE: render full-page */
#define FW_BLOCK_FLAG_INDENT  (1u << 1)  /* PARAGRAPH: first-line indent */

/* ── FwBlock — GObject for use in GListModel ──────────────────────── */

#define FW_TYPE_BLOCK (fw_block_get_type ())

G_DECLARE_FINAL_TYPE (FwBlock, fw_block, FW, BLOCK, GObject)

FwBlock     *fw_block_new            (FwBlockKind  kind,
                                      int          level,
                                      const char  *text,
                                      const char  *image_id,
                                      const char  *anchor_id,
                                      guint        flags);

FwBlockKind  fw_block_get_kind       (FwBlock *self);
int          fw_block_get_level      (FwBlock *self);
const char  *fw_block_get_text       (FwBlock *self);
const char  *fw_block_get_image_id   (FwBlock *self);
const char  *fw_block_get_anchor_id  (FwBlock *self);
guint        fw_block_get_flags      (FwBlock *self);
void         fw_block_set_flags      (FwBlock *self, guint flags);

/* ── Reflow TOC entry ─────────────────────────────────────────────── */

#define FW_TYPE_REFLOW_TOC_ITEM (fw_reflow_toc_item_get_type ())

G_DECLARE_FINAL_TYPE (FwReflowTocItem, fw_reflow_toc_item,
                      FW, REFLOW_TOC_ITEM, GObject)

FwReflowTocItem *fw_reflow_toc_item_new      (const char *title,
                                              const char *anchor_id);
const char      *fw_reflow_toc_item_get_title     (FwReflowTocItem *self);
const char      *fw_reflow_toc_item_get_anchor_id (FwReflowTocItem *self);

/* ── FwReflowDocument interface ───────────────────────────────────── */

#define FW_TYPE_REFLOW_DOCUMENT (fw_reflow_document_get_type ())

G_DECLARE_INTERFACE (FwReflowDocument, fw_reflow_document,
                     FW, REFLOW_DOCUMENT, GObject)

struct _FwReflowDocumentInterface {
  GTypeInterface parent_iface;

  gboolean     (*open)             (FwReflowDocument *self,
                                    const char       *path,
                                    GError          **error);
  void         (*close)            (FwReflowDocument *self);

  /* Hot path — bound directly to GtkListView. */
  GListModel  *(*get_block_model)  (FwReflowDocument *self);

  GdkTexture  *(*get_image)        (FwReflowDocument *self,
                                    const char       *image_id);

  GListModel  *(*get_toc)          (FwReflowDocument *self);
  guint        (*find_block_by_anchor) (FwReflowDocument *self,
                                        const char       *anchor_id);

  /* GArray of (block_index, char_offset, length) triples — Phase 6. */
  GArray      *(*search)           (FwReflowDocument *self,
                                    const char       *needle);

  GHashTable  *(*get_metadata)     (FwReflowDocument *self);
};

gboolean     fw_reflow_document_open                (FwReflowDocument *self,
                                                     const char       *path,
                                                     GError          **error);
void         fw_reflow_document_close               (FwReflowDocument *self);
GListModel  *fw_reflow_document_get_block_model     (FwReflowDocument *self);
GdkTexture  *fw_reflow_document_get_image           (FwReflowDocument *self,
                                                     const char       *image_id);
GListModel  *fw_reflow_document_get_toc             (FwReflowDocument *self);
guint        fw_reflow_document_find_block_by_anchor(FwReflowDocument *self,
                                                     const char       *anchor_id);
GArray      *fw_reflow_document_search              (FwReflowDocument *self,
                                                     const char       *needle);
GHashTable  *fw_reflow_document_get_metadata        (FwReflowDocument *self);

/* ── Path → reflow-eligible? ──────────────────────────────────────── */

/* Returns TRUE when the file at `path` should be opened with the reflow
 * pipeline rather than the fixed-layout one. Phase 1 = .txt only. */
gboolean     fw_reflow_path_is_supported (const char *path);

/* ── Factory ──────────────────────────────────────────────────────── */

FwReflowDocument *fw_reflow_document_new_for_path (const char  *path,
                                                   GError     **error);

G_END_DECLS
