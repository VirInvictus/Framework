/* fw-document.h — Abstract document interface
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include <cairo.h>

G_BEGIN_DECLS

/* ── TOC node ─────────────────────────────────────────────────────── */

typedef struct _FwTocNode FwTocNode;

struct _FwTocNode {
  char        *title;
  int          page;       /* 0-based destination page, or -1 if none */
  FwTocNode   *children;   /* first child (linked list via ->next) */
  FwTocNode   *next;       /* next sibling */
};

FwTocNode *fw_toc_node_new    (const char *title, int page);
void       fw_toc_node_free   (FwTocNode  *node);

/* ── Link rectangle ───────────────────────────────────────────────── */

typedef enum {
  FW_LINK_INTERNAL,  /* jump to a page within the document */
  FW_LINK_EXTERNAL,  /* open a URI */
} FwLinkType;

typedef struct {
  FwLinkType  type;
  double      x0, y0, x1, y1;   /* rectangle in page coords (points) */
  int         dest_page;          /* for INTERNAL links (0-based) */
  char       *uri;                /* for EXTERNAL links (owned) */
} FwLink;

FwLink *fw_link_new_internal (double x0, double y0, double x1, double y1,
                              int dest_page);
FwLink *fw_link_new_external (double x0, double y0, double x1, double y1,
                              const char *uri);
void    fw_link_free         (FwLink *link);
void    fw_link_free_indirect (gpointer data);  /* for g_array_set_clear_func */

/* ── Embedded file attachment ────────────────────────────────────── */

typedef struct {
  char     *name;          /* sanitized basename — safe to join with output dir */
  char     *mime_type;     /* may be NULL */
  gint64    size;          /* in bytes (-1 if unknown) */
  gpointer  backend_data;  /* opaque cookie passed back to save_attachment */
} FwAttachment;

void fw_attachment_free          (FwAttachment *a);
void fw_attachment_free_indirect (gpointer      data);  /* for g_array set_clear_func */

/* ── Selection granularity ───────────────────────────────────────── */

typedef enum {
  FW_SELECT_WORD,
  FW_SELECT_LINE,
} FwSelectGranularity;

/* ── Page-coordinate rectangle (points) ──────────────────────────── */

typedef struct {
  double x0, y0, x1, y1;
} FwRect;

/* ── Search hit ───────────────────────────────────────────────────── */

typedef struct {
  int    page;                    /* 0-based */
  double x0, y0, x1, y1;         /* rectangle in page coords (points) */
} FwSearchHit;

/* ── FwDocument interface ─────────────────────────────────────────── */

#define FW_TYPE_DOCUMENT (fw_document_get_type ())

G_DECLARE_INTERFACE (FwDocument, fw_document, FW, DOCUMENT, GObject)

struct _FwDocumentInterface {
  GTypeInterface parent_iface;

  gboolean         (*open)           (FwDocument   *self,
                                      const char   *path,
                                      GError      **error);
  void             (*close)          (FwDocument   *self);

  int              (*get_page_count) (FwDocument   *self);
  void             (*get_page_size)  (FwDocument   *self,
                                      int           page,
                                      double       *width,
                                      double       *height);

  cairo_surface_t *(*render_page)    (FwDocument   *self,
                                      int           page,
                                      double        zoom,
                                      int           rotation);

  FwTocNode       *(*get_toc)        (FwDocument   *self);

  GArray          *(*search)         (FwDocument   *self,
                                      const char   *text,
                                      int           page);

  char            *(*get_text)       (FwDocument   *self,
                                      int           page,
                                      double        x0,
                                      double        y0,
                                      double        x1,
                                      double        y1);

  GArray          *(*get_links)      (FwDocument   *self,
                                      int           page);

  /* Page handle API — allows separating page loading (I/O) from rendering.
   * Backends that support this return a lightweight parsed page object from
   * open_page, which can be passed to render_page_from_handle to skip the
   * load step. close_page releases the handle. */
  gpointer         (*open_page)     (FwDocument   *self,
                                      int           page);
  void             (*close_page)    (FwDocument   *self,
                                      gpointer      handle);
  cairo_surface_t *(*render_page_from_handle)
                                     (FwDocument   *self,
                                      gpointer      handle,
                                      double        zoom,
                                      int           rotation);

  /* Render cancellation — called when the cache aborts during high-velocity
   * scrubbing. Backends can use this to bail out of in-progress renders. */
  void             (*cancel_render) (FwDocument   *self);

  /* Embedded files (PDF /EmbeddedFiles name tree). Returns a GArray of
   * `FwAttachment *` (caller frees with g_array_unref + per-element
   * fw_attachment_free) or NULL when the format does not support
   * attachments. Save writes one attachment to a destination path; the
   * caller is responsible for choosing a safe path under a user-selected
   * directory. */
  GArray          *(*get_attachments)  (FwDocument   *self);
  gboolean         (*save_attachment)  (FwDocument   *self,
                                        FwAttachment *attachment,
                                        const char   *output_path,
                                        GError      **error);

  /* Document metadata. Returns a GHashTable<gchar*, gchar*> of free-form
   * key/value pairs (both owned by the table; destroy with
   * g_hash_table_unref). NULL when the backend exposes no metadata.
   * Canonical keys when present: "title", "author", "subject", "keywords",
   * "creator", "producer", "creation-date", "modification-date", "format",
   * "encryption". Dates are pre-formatted human-readable strings. */
  GHashTable      *(*get_metadata)     (FwDocument   *self);

  /* Snap a click point to a word- or line-sized selection rectangle.
   * Returns TRUE on success and writes the snapped page-coordinate
   * bounding box (in points) to the out parameters; returns FALSE if
   * the click missed all glyphs or the backend doesn't support
   * structured-text snap (DjVu, CBR — fall back to drag selection).
   * Backed by the cached fz_stext_page from Phase 11 Tier 1, so the
   * call is constant-time after first text access on a page. */
  gboolean         (*select_at)        (FwDocument   *self,
                                         int           page,
                                         double        x,
                                         double        y,
                                         FwSelectGranularity gran,
                                         double       *out_x0,
                                         double       *out_y0,
                                         double       *out_x1,
                                         double       *out_y1);

  /* Compute per-line highlight rectangles for a text selection from
   * point (x0,y0) to point (x1,y1), following reading order. Returns a
   * GArray of FwRect (page-point coordinates). The caller frees with
   * g_array_unref. NULL means the backend doesn't support stext-based
   * selection (DjVu, CBR — view falls back to drawing the bounding
   * rectangle). Backed by the same cached fz_stext_page as select_at. */
  GArray          *(*get_selection_quads) (FwDocument *self,
                                            int         page,
                                            double      x0,
                                            double      y0,
                                            double      x1,
                                            double      y1);

  /* Compute the page's inked-content bounding box in document points —
   * the union of all glyph rectangles, used by the margin-crop feature
   * to figure out how much whitespace to trim. Returns TRUE on success
   * with the bbox in *out_x0..*out_y1; FALSE when the backend doesn't
   * support stext-based content detection (DjVu, CBR), when the page
   * has no inked content, or when the cached stext isn't available. */
  gboolean         (*get_content_bbox)  (FwDocument *self,
                                          int         page,
                                          double     *out_x0,
                                          double     *out_y0,
                                          double     *out_x1,
                                          double     *out_y1);
};

/* Public API — delegates to interface vtable */
gboolean         fw_document_open           (FwDocument   *self,
                                             const char   *path,
                                             GError      **error);
void             fw_document_close          (FwDocument   *self);
int              fw_document_get_page_count (FwDocument   *self);
void             fw_document_get_page_size  (FwDocument   *self,
                                             int           page,
                                             double       *width,
                                             double       *height);
cairo_surface_t *fw_document_render_page    (FwDocument   *self,
                                             int           page,
                                             double        zoom,
                                             int           rotation);
FwTocNode       *fw_document_get_toc        (FwDocument   *self);
GArray          *fw_document_search         (FwDocument   *self,
                                             const char   *text,
                                             int           page);
char            *fw_document_get_text       (FwDocument   *self,
                                             int           page,
                                             double        x0,
                                             double        y0,
                                             double        x1,
                                             double        y1);
GArray          *fw_document_get_links      (FwDocument   *self,
                                             int           page);

/* Page handle API */
gpointer         fw_document_open_page      (FwDocument   *self,
                                             int           page);
void             fw_document_close_page     (FwDocument   *self,
                                             gpointer      handle);
cairo_surface_t *fw_document_render_page_from_handle
                                            (FwDocument   *self,
                                             gpointer      handle,
                                             double        zoom,
                                             int           rotation);
void             fw_document_cancel_render  (FwDocument   *self);
GArray          *fw_document_get_attachments(FwDocument   *self);
gboolean         fw_document_save_attachment(FwDocument   *self,
                                             FwAttachment *attachment,
                                             const char   *output_path,
                                             GError      **error);
GHashTable      *fw_document_get_metadata   (FwDocument   *self);
gboolean         fw_document_select_at      (FwDocument   *self,
                                             int           page,
                                             double        x,
                                             double        y,
                                             FwSelectGranularity gran,
                                             double       *out_x0,
                                             double       *out_y0,
                                             double       *out_x1,
                                             double       *out_y1);
GArray          *fw_document_get_selection_quads (FwDocument *self,
                                                   int         page,
                                                   double      x0,
                                                   double      y0,
                                                   double      x1,
                                                   double      y1);
gboolean         fw_document_get_content_bbox    (FwDocument *self,
                                                   int         page,
                                                   double     *out_x0,
                                                   double     *out_y0,
                                                   double     *out_x1,
                                                   double     *out_y1);

/* ── Factory ──────────────────────────────────────────────────────── */

FwDocument *fw_document_new_for_path (const char  *path,
                                     GError     **error);

G_END_DECLS
