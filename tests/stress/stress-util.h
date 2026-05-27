/* tests/stress/stress-util.h — shared helpers for the stress/bench targets
 *
 * Small utilities that every stress/bench main needs: RSS sampling, a
 * main-loop pump, and a file-existence check. Kept here as static inline
 * so each target gets its own copy without a separate compilation unit.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/resource.h>

/* Peak resident set size in MB (high-water mark for the process). */
static inline long
rss_peak_mb (void)
{
  struct rusage ru;
  getrusage (RUSAGE_SELF, &ru);
  return ru.ru_maxrss / 1024;
}

/* Current resident set size in MB, read from /proc/self/status. -1 on
 * error. Unlike rss_peak_mb this falls after memory is released, so it's
 * the right signal for "did we settle back down" leak checks. */
static inline long
rss_current_mb (void)
{
  FILE *f = fopen ("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long kb = -1;
  while (fgets (line, sizeof line, f)) {
    if (strncmp (line, "VmRSS:", 6) == 0) {
      kb = strtol (line + 6, NULL, 10);
      break;
    }
  }
  fclose (f);
  return kb < 0 ? -1 : kb / 1024;
}

/* Pump the default main context for `ms` milliseconds so render workers
 * can post their results back and idle callbacks fire. */
static inline void
spin_main_loop (int ms)
{
  GMainContext *ctx = g_main_context_default ();
  gint64 deadline = g_get_monotonic_time () + (gint64) ms * 1000;
  while (g_get_monotonic_time () < deadline)
    g_main_context_iteration (ctx, FALSE);
}

static inline gboolean
file_exists (const char *path)
{
  return g_file_test (path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR);
}
