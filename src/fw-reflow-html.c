/* fw-reflow-html.c — Shared HTML-emit helpers for the WebView path.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-reflow-html.h"

/* The :root custom properties are the typography/theme knobs. These
 * defaults (serif body, light theme) render a sane book on first paint;
 * the window overrides them live from GSettings via
 * fw_webview_set_reading_style (font, size, line-height, theme colors). */
static const char READING_CSS[] =
  ":root {"
  "  --fg: #1a1a1a; --bg: #fdfdfb; --link: #1a5fb4;"
  "  --body-font: 'Crimson Pro', Georgia, 'Times New Roman', serif;"
  "  --mono-font: ui-monospace, monospace;"
  "  --font-size: 13pt; --line-height: 1.55; --measure: 38em;"
  "}"
  /* !important on the sovereignty rules: with publisher CSS honoured
   * (EPUB, v0.79) the user's theme colors, chosen typography, and the
   * reading column must still win over book-level body/html styling.
   * Publisher rules keep everything more specific (headings, tables,
   * drop caps); these guard only the reading surface itself. */
  "html { background: var(--bg) !important; color: var(--fg) !important; }"
  "body { max-width: var(--measure) !important;"
  "       margin: 1.5em auto !important; padding: 0 1.5em !important;"
  "       background: var(--bg) !important; color: var(--fg) !important;"
  "       font-family: var(--body-font) !important;"
  "       font-size: var(--font-size) !important;"
  "       line-height: var(--line-height) !important; }"
  "a { color: var(--link) !important; }"
  "section[data-spine] { margin: 0 0 2em 0; }"
  "section.cover { display: flex; align-items: center; justify-content: center;"
                  "min-height: 100vh; margin: 0; padding: 0; max-width: none; }"
  "section.cover img { max-width: 100%; max-height: 96vh; height: auto; "
                      "object-fit: contain; }"
  "h1, h2, h3 { line-height: 1.2; }"
  "img { max-width: 100%; height: auto; }"
  "hr { border: none; border-top: 1px solid color-mix(in srgb, currentColor 25%, transparent);"
  "     margin: 1.6em auto; width: 40%; }"
  /* Code: a subtle theme-adaptive tint (color-mix off currentColor keeps
   * it readable in every theme). Inline code hugs the text; fenced
   * blocks scroll horizontally rather than forcing the column wide. */
  "code, pre { font-family: var(--mono-font); }"
  "code { background: color-mix(in srgb, currentColor 8%, transparent);"
  "       padding: 0.1em 0.3em; border-radius: 4px; font-size: 0.9em; }"
  "pre { background: color-mix(in srgb, currentColor 6%, transparent);"
  "      padding: 0.8em 1em; border-radius: 6px; overflow-x: auto;"
  "      line-height: 1.45; }"
  "pre code { background: none; padding: 0; border-radius: 0; font-size: 0.88em; }"
  /* GFM tables: collapsed borders, padded cells, a faint header fill. */
  "table { border-collapse: collapse; margin: 1em 0; width: 100%; }"
  "th, td { border: 1px solid color-mix(in srgb, currentColor 22%, transparent);"
  "         padding: 0.4em 0.7em; }"
  "th { background: color-mix(in srgb, currentColor 8%, transparent);"
  "     text-align: left; }"
  /* GFM task lists: drop the bullet, align the checkbox. */
  "li.task-list-item { list-style: none; margin-left: -1.3em; }"
  "li.task-list-item input { margin-right: 0.5em; }"
  "blockquote { border-left: 3px solid currentColor; "
              "padding-left: 1em; opacity: 0.85; "
              "margin: 1em 0 1em 1em; }";

const char *
fw_reflow_reading_css (void)
{
  return READING_CSS;
}

xmlNodePtr
fw_reflow_html_find_body (xmlNodePtr root)
{
  for (xmlNodePtr n = root; n; n = n->next) {
    if (n->type == XML_ELEMENT_NODE &&
        xmlStrcasecmp (n->name, BAD_CAST "body") == 0)
      return n;
    xmlNodePtr deeper = n->children ? fw_reflow_html_find_body (n->children) : NULL;
    if (deeper) return deeper;
  }
  return NULL;
}

static gboolean
is_stylesheet_link (xmlNodePtr node)
{
  if (xmlStrcasecmp (node->name, BAD_CAST "link") != 0)
    return FALSE;
  xmlChar *rel = xmlGetProp (node, BAD_CAST "rel");
  gboolean is = rel && g_ascii_strcasecmp ((const char *) rel, "stylesheet") == 0;
  if (rel) xmlFree (rel);
  return is;
}

/* Elements that can execute or embed active content.  No legitimate
 * ebook needs any of these; the WebView runs with JavaScript enabled
 * (our position-tracking scripts need it), so they must not survive
 * into the stitched document. */
static gboolean
is_active_element (xmlNodePtr node)
{
  return xmlStrcasecmp (node->name, BAD_CAST "script") == 0 ||
         xmlStrcasecmp (node->name, BAD_CAST "iframe") == 0 ||
         xmlStrcasecmp (node->name, BAD_CAST "object") == 0 ||
         xmlStrcasecmp (node->name, BAD_CAST "embed")  == 0;
}

/* TRUE when a URL-valued attribute resolves to a script scheme.  HTML's
 * URL parser strips tab / LF / CR anywhere in the input before scheme
 * resolution, so "java\nscript:" executes just like "javascript:" —
 * mirror that here rather than doing a naive prefix compare.  Exported
 * as fw_reflow_url_is_script for backends that emit their own anchors
 * (FB2) instead of going through fw_reflow_html_process. */
gboolean
fw_reflow_url_is_script (const char *value)
{
  const char *p = value;
  while (*p && (guchar) *p <= 0x20)  /* leading whitespace/controls */
    p++;
  char scheme[16];
  gsize n = 0;
  for (; *p; p++) {
    if (*p == '\t' || *p == '\n' || *p == '\r')
      continue;
    if (*p == ':') {
      scheme[n] = '\0';
      return g_ascii_strcasecmp (scheme, "javascript") == 0 ||
             g_ascii_strcasecmp (scheme, "vbscript") == 0;
    }
    if (n >= sizeof scheme - 1)
      return FALSE;                  /* longer than any script scheme */
    if (!g_ascii_isalnum (*p) && *p != '+' && *p != '-' && *p != '.')
      return FALSE;                  /* not a scheme — relative URL etc. */
    scheme[n++] = *p;
  }
  return FALSE;
}

static gboolean
is_url_attr (const xmlChar *name)
{
  static const char *const url_attrs[] = {
    "href", "src", "action", "formaction", "poster", "data", "background",
  };
  for (gsize i = 0; i < G_N_ELEMENTS (url_attrs); i++)
    if (g_ascii_strcasecmp ((const char *) name, url_attrs[i]) == 0)
      return TRUE;
  return FALSE;
}

/* Drop inline event handlers (any on* attribute) and script-scheme
 * URLs from one element.  Removal-safe iteration: xmlRemoveProp frees
 * the attr, so the next pointer is saved first. */
static void
scrub_attributes (xmlNodePtr node)
{
  xmlAttr *a = node->properties;
  while (a) {
    xmlAttr *next = a->next;
    gboolean drop = FALSE;
    if (g_ascii_strncasecmp ((const char *) a->name, "on", 2) == 0) {
      drop = TRUE;
    } else if (is_url_attr (a->name)) {
      xmlChar *val = xmlNodeListGetString (node->doc, a->children, 1);
      drop = val && fw_reflow_url_is_script ((const char *) val);
      if (val) xmlFree (val);
    }
    if (drop)
      xmlRemoveProp (a);
    a = next;
  }
}

void
fw_reflow_html_process (xmlNodePtr body, const char *doc_id,
                        FwReflowImgResolver resolver, gpointer user_data)
{
  xmlNodePtr c = body->children;
  while (c) {
    xmlNodePtr next = c->next;
    if (c->type == XML_ELEMENT_NODE) {
      if (is_active_element (c) || is_stylesheet_link (c)) {
        xmlUnlinkNode (c);
        xmlFreeNode  (c);
      } else {
        scrub_attributes (c);
        if (xmlStrcasecmp (c->name, BAD_CAST "img") == 0) {
          g_autofree char *id = resolver ? resolver (c, user_data) : NULL;
          if (id) {
            g_autofree char *new_src =
              g_strdup_printf ("framework-img://%s/%s", doc_id, id);
            xmlSetProp (c, BAD_CAST "src", BAD_CAST new_src);
          }
        }
        fw_reflow_html_process (c, doc_id, resolver, user_data);
      }
    }
    c = next;
  }
}
