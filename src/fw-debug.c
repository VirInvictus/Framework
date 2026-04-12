/* fw-debug.c — Runtime debug tracing implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-debug.h"

#include <stdarg.h>
#include <stdio.h>

static volatile gint debug_active = 0;

void
fw_debug_init (void)
{
  const char *env = g_getenv ("FW_DEBUG");
  if (env && env[0] != '\0' && env[0] != '0')
    g_atomic_int_set (&debug_active, 1);
}

gboolean
fw_debug_enabled (void)
{
  return g_atomic_int_get (&debug_active) != 0;
}

void
fw_debug_print (const char *domain, const char *fmt, ...)
{
  gint64 now = g_get_monotonic_time ();
  double secs = (double) now / 1000000.0;

  va_list args;
  va_start (args, fmt);

  g_autofree char *msg = g_strdup_vprintf (fmt, args);
  va_end (args);

  /* Thread-safe: fprintf to stderr is line-buffered and atomic for
   * lines under PIPE_BUF (4096 on Linux). */
  fprintf (stderr, "[%10.4f] [%-6s] %s\n", secs, domain, msg);
}
