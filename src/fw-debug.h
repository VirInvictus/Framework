/* fw-debug.h — Runtime debug tracing
 *
 * Enable with: FW_DEBUG=1 ./builddir/framework file.pdf
 *
 * Logs timestamped events for document loading, page rendering, cache
 * operations, view lifecycle, and window actions.  Zero overhead when
 * disabled — the check is a single atomic load per call site.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Call once at startup (e.g. in main or application init).
 * Reads FW_DEBUG environment variable. */
void fw_debug_init (void);

/* Returns TRUE if debug tracing is active. */
gboolean fw_debug_enabled (void);

/* Core trace macro — prints [timestamp] [domain] message to stderr.
 * Compiles to nothing when FW_DEBUG is not set at runtime. */
#define FW_TRACE(domain, fmt, ...) \
  G_STMT_START { \
    if (G_UNLIKELY (fw_debug_enabled ())) \
      fw_debug_print (domain, fmt, ##__VA_ARGS__); \
  } G_STMT_END

/* Domain-specific convenience macros */
#define FW_TRACE_DOC(fmt, ...)    FW_TRACE ("doc",    fmt, ##__VA_ARGS__)
#define FW_TRACE_PDF(fmt, ...)    FW_TRACE ("pdf",    fmt, ##__VA_ARGS__)
#define FW_TRACE_DJVU(fmt, ...)   FW_TRACE ("djvu",   fmt, ##__VA_ARGS__)
#define FW_TRACE_CACHE(fmt, ...)  FW_TRACE ("cache",  fmt, ##__VA_ARGS__)
#define FW_TRACE_VIEW(fmt, ...)   FW_TRACE ("view",   fmt, ##__VA_ARGS__)
#define FW_TRACE_WINDOW(fmt, ...) FW_TRACE ("window", fmt, ##__VA_ARGS__)
#define FW_TRACE_MEM(fmt, ...)    FW_TRACE ("mem",    fmt, ##__VA_ARGS__)

/* Internal — do not call directly, use FW_TRACE macros. */
void fw_debug_print (const char *domain, const char *fmt, ...)
  G_GNUC_PRINTF (2, 3);

G_END_DECLS
