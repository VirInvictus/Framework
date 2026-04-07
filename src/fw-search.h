/* fw-search.h — Search controller
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "fw-document.h"

G_BEGIN_DECLS

#define FW_TYPE_SEARCH (fw_search_get_type ())

G_DECLARE_FINAL_TYPE (FwSearch, fw_search, FW, SEARCH, GObject)

FwSearch *fw_search_new          (void);
void      fw_search_set_document (FwSearch   *self,
                                  FwDocument *document);
void      fw_search_find         (FwSearch   *self,
                                  const char *text);
void      fw_search_next         (FwSearch   *self);
void      fw_search_prev         (FwSearch   *self);
int       fw_search_get_count    (FwSearch   *self);
int       fw_search_get_current  (FwSearch   *self);

G_END_DECLS
