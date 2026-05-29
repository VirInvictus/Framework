/* fw-reflow-html.h — Shared helpers for the WebView (produce_html) path.
 *
 * The reading stylesheet and the libxml2 subtree pass (strip scripts /
 * stylesheet links, rewrite <img> to the framework-img: scheme) are
 * shared by every reflow backend that stitches HTML for the WebView
 * (EPUB, MOBI/AZW3, and FB2/TXT to come). The per-backend difference is
 * only how an <img> maps to an image id, supplied as a resolver callback.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>
#include <libxml/tree.h>

G_BEGIN_DECLS

/* The reading stylesheet injected into every stitched document. Defines
 * the :root custom properties (font, size, line-height, measure, theme
 * colors) the window overrides live via fw_webview_set_reading_style. */
const char *fw_reflow_reading_css (void);

/* Locate <body> in a parsed HTML tree, or NULL. */
xmlNodePtr  fw_reflow_html_find_body (xmlNodePtr root);

/* Resolve an <img> element to an image id (newly-allocated, caller frees
 * via g_free), or NULL to leave the element's src untouched. */
typedef char *(*FwReflowImgResolver) (xmlNodePtr img, gpointer user_data);

/* Walk `body`: drop <script> and stylesheet <link> elements, and rewrite
 * each <img>'s src to `framework-img://<doc_id>/<id>` where `id` comes
 * from `resolver`. Recurses into the whole subtree. */
void        fw_reflow_html_process (xmlNodePtr           body,
                                    const char          *doc_id,
                                    FwReflowImgResolver  resolver,
                                    gpointer             user_data);

G_END_DECLS
