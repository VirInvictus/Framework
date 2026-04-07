/* main.c — Entry point for Framework
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-config.h"
#include "fw-application.h"

int
main (int argc, char *argv[])
{
  g_autoptr (FwApplication) app = fw_application_new ();
  return g_application_run (G_APPLICATION (app), argc, argv);
}
