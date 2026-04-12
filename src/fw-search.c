/* fw-search.c — Search controller
 *
 * Searches all pages and maintains a flat list of hits with a current index.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-search.h"

struct _FwSearch {
  GObject      parent_instance;

  FwDocument  *document;  /* owned ref */
  GArray      *hits;      /* FwSearchHit[] across all pages */
  int          current;   /* index into hits, or -1 */
};

G_DEFINE_FINAL_TYPE (FwSearch, fw_search, G_TYPE_OBJECT)

FwSearch *
fw_search_new (void)
{
  return g_object_new (FW_TYPE_SEARCH, NULL);
}

void
fw_search_set_document (FwSearch *self, FwDocument *document)
{
  g_return_if_fail (FW_IS_SEARCH (self));
  g_set_object (&self->document, document);

  if (self->hits) {
    g_array_unref (self->hits);
    self->hits = NULL;
  }
  self->current = -1;
}

void
fw_search_find (FwSearch *self, const char *text)
{
  g_return_if_fail (FW_IS_SEARCH (self));

  if (self->hits) {
    g_array_unref (self->hits);
    self->hits = NULL;
  }
  self->current = -1;

  if (!self->document || !text || !text[0])
    return;

  self->hits = g_array_new (FALSE, FALSE, sizeof (FwSearchHit));
  int pages = fw_document_get_page_count (self->document);

  for (int i = 0; i < pages; i++) {
    GArray *page_hits = fw_document_search (self->document, text, i);
    if (page_hits && page_hits->len > 0)
      g_array_append_vals (self->hits, page_hits->data, page_hits->len);
    if (page_hits)
      g_array_unref (page_hits);
  }

  if (self->hits->len > 0)
    self->current = 0;
}

void
fw_search_next (FwSearch *self)
{
  g_return_if_fail (FW_IS_SEARCH (self));
  if (!self->hits || self->hits->len == 0)
    return;
  self->current = (self->current + 1) % (int) self->hits->len;
}

void
fw_search_prev (FwSearch *self)
{
  g_return_if_fail (FW_IS_SEARCH (self));
  if (!self->hits || self->hits->len == 0)
    return;
  self->current--;
  if (self->current < 0)
    self->current = (int) self->hits->len - 1;
}

int
fw_search_get_count (FwSearch *self)
{
  g_return_val_if_fail (FW_IS_SEARCH (self), 0);
  return self->hits ? (int) self->hits->len : 0;
}

int
fw_search_get_current (FwSearch *self)
{
  g_return_val_if_fail (FW_IS_SEARCH (self), -1);
  return self->current;
}

static void
fw_search_dispose (GObject *object)
{
  FwSearch *self = FW_SEARCH (object);
  g_clear_object (&self->document);
  G_OBJECT_CLASS (fw_search_parent_class)->dispose (object);
}

static void
fw_search_finalize (GObject *object)
{
  FwSearch *self = FW_SEARCH (object);
  if (self->hits)
    g_array_unref (self->hits);
  G_OBJECT_CLASS (fw_search_parent_class)->finalize (object);
}

static void
fw_search_class_init (FwSearchClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose  = fw_search_dispose;
  G_OBJECT_CLASS (klass)->finalize = fw_search_finalize;
}

static void
fw_search_init (FwSearch *self)
{
  self->current = -1;
}
