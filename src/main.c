/* main.c — Entry point for Framework
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-config.h"
#include "fw-application.h"

#include <stdio.h>
#include <string.h>

int
main (int argc, char *argv[])
{
  for (int i = 1; i < argc; i++) {
    if (strcmp (argv[i], "--version") == 0 ||
        strcmp (argv[i], "-v") == 0) {
      printf ("Framework %s\n", APP_VERSION);
      return 0;
    }
  }

  g_autoptr (FwApplication) app = fw_application_new ();
  return g_application_run (G_APPLICATION (app), argc, argv);
}
