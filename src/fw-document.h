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

/* ── Factory ──────────────────────────────────────────────────────── */

FwDocument *fw_document_new_for_path (const char  *path,
                                     GError     **error);

G_END_DECLS
