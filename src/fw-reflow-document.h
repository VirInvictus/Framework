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

  GListModel  *(*get_toc)          (FwReflowDocument *self);

  GHashTable  *(*get_metadata)     (FwReflowDocument *self);

  /* Render the document as a single self-contained HTML string for the
   * WebKitGTK reader.  Backends that implement this method opt in to
   * the WebView render path; the window dispatches them there instead
   * of through the FwReflowView GtkListView pipeline.
   *
   * `doc_id` is the per-FwWebView identifier (host portion of
   * `framework-img://<doc-id>/<image-id>` URIs); embed it directly in
   * the rewritten image src attributes so WebKit's scheme handler can
   * route resolutions back to the right view.
   *
   * On success: *out_html is g_strdup'd HTML (caller frees); *out_images
   * is a fresh GHashTable<gchar* image_id, GBytes*> the caller hands to
   * fw_webview_load_html (which takes a hash-table ref).  Either may be
   * NULL when the document has no images.
   *
   * On failure: returns FALSE with *error set; the out-params are
   * untouched. */
  gboolean     (*produce_html)     (FwReflowDocument  *self,
                                    const char        *doc_id,
                                    char             **out_html,
                                    GHashTable       **out_images,
                                    GError           **error);

  /* Optional.  Path-keyed resource table (gchar* archive path → GBytes*)
   * backing `framework-img://<doc-id>/res/<path>` URIs: publisher CSS,
   * fonts (already de-obfuscated), and anything relative url()/@import
   * references inside those stylesheets resolve to.  Transfer none —
   * the table stays owned by the document; fw_webview_load_html takes
   * its own ref.  NULL when the format has no path-addressed resources
   * (every format except EPUB today). */
  GHashTable  *(*get_resources)    (FwReflowDocument *self);
};


gboolean     fw_reflow_document_open                (FwReflowDocument *self,
                                                     const char       *path,
                                                     GError          **error);
void         fw_reflow_document_close               (FwReflowDocument *self);
GListModel  *fw_reflow_document_get_toc             (FwReflowDocument *self);

GHashTable  *fw_reflow_document_get_metadata        (FwReflowDocument *self);

/* Optional: returns TRUE iff the backend implements `produce_html`.  The
 * window dispatch uses this to decide whether to route the document to
 * the WebView render path. */
gboolean     fw_reflow_document_supports_html       (FwReflowDocument *self);

/* Returns TRUE on success; out-params set per the vtable contract.  When
 * the backend has no produce_html implementation, returns FALSE with
 * G_IO_ERROR_NOT_SUPPORTED. */
gboolean     fw_reflow_document_produce_html        (FwReflowDocument  *self,
                                                     const char        *doc_id,
                                                     char             **out_html,
                                                     GHashTable       **out_images,
                                                     GError           **error);

/* Path-keyed resource table for the WebView's /res/ URIs, or NULL.
 * Transfer none. */
GHashTable  *fw_reflow_document_get_resources       (FwReflowDocument  *self);

/* ── Path → reflow-eligible? ──────────────────────────────────────── */

/* Returns TRUE when the file at `path` should be opened with the reflow
 * pipeline rather than the fixed-layout one. Phase 1 = .txt only. */
gboolean     fw_reflow_path_is_supported (const char *path);

/* ── Factory ──────────────────────────────────────────────────────── */

FwReflowDocument *fw_reflow_document_new_for_path (const char  *path,
                                                   GError     **error);

G_END_DECLS
