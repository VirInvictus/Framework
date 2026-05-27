/* fw-search.c — Async search controller
 *
 * Searches all pages on a background thread, emitting incremental hits as
 * pages are scanned. The UI thread is never blocked — even on a 1000-page
 * document, the search bar stays responsive and the user can navigate
 * matches as they appear. The scan starts at the caller's start_page so
 * matches near where the user is reading appear first.
 *
 * Threading: the worker thread (a GThread) calls fw_document_search() per
 * page and posts the result back to the main loop via g_idle_add_full(); all
 * GObject state mutations happen on the main thread, so signal emission and
 * GArray writes are race-free.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-search.h"
#include "fw-debug.h"

struct _FwSearch {
  GObject       parent_instance;

  FwDocument   *document;       /* owned ref */
  GArray       *hits;           /* FwSearchHit[] across all pages */
  int           current;        /* index into hits, or -1 */

  /* Worker state — read/written only from the main thread. The worker
   * thread reads `query`, `start_page`, and `generation` (atomic).
   * Cancellation works by bumping `generation`; the worker compares
   * its captured generation against the live one and bails when they
   * differ. */
  GThread      *worker;
  char         *query;          /* owned, set before worker starts */
  int           start_page;
  guint         finish_idle_id; /* main-loop id for the finalize idle */

  /* Generation counter — incremented on every find()/clear()/
   * set_document() so any still-pending idle messages from the
   * previous scan are dropped, AND any still-running worker thread
   * recognizes itself as superseded and bails. */
  guint         generation;
};

enum {
  SIGNAL_HITS_CHANGED,
  SIGNAL_CURRENT_CHANGED,
  SIGNAL_SEARCH_FINISHED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (FwSearch, fw_search, G_TYPE_OBJECT)

/* ── Worker ───────────────────────────────────────────────────────── */

typedef struct {
  FwSearch    *search;          /* strong ref while in flight */
  guint        generation;      /* must match self->generation to be applied */
  int          page;            /* page these hits came from */
  GArray      *page_hits;       /* may be NULL or empty */
  gboolean     finished;        /* TRUE → "all pages scanned" message */
} HitMessage;

static void
hit_message_free (gpointer data)
{
  HitMessage *msg = data;
  if (msg->page_hits)
    g_array_unref (msg->page_hits);
  g_object_unref (msg->search);
  g_free (msg);
}

static gboolean
hit_message_apply (gpointer data)
{
  HitMessage *msg = data;
  FwSearch   *self = msg->search;

  if (msg->generation != self->generation)
    return G_SOURCE_REMOVE;       /* stale — a newer search has started */

  if (msg->finished) {
    self->finish_idle_id = 0;
    g_signal_emit (self, signals[SIGNAL_SEARCH_FINISHED], 0);
    return G_SOURCE_REMOVE;
  }

  if (msg->page_hits && msg->page_hits->len > 0) {
    gboolean was_empty = (self->hits->len == 0);
    g_array_append_vals (self->hits,
                         msg->page_hits->data, msg->page_hits->len);
    g_signal_emit (self, signals[SIGNAL_HITS_CHANGED], 0);
    if (was_empty) {
      self->current = 0;
      g_signal_emit (self, signals[SIGNAL_CURRENT_CHANGED], 0);
    }
  }

  return G_SOURCE_REMOVE;
}

static void
post_hits (FwSearch *self, guint generation, int page, GArray *page_hits)
{
  HitMessage *msg = g_new0 (HitMessage, 1);
  msg->search     = g_object_ref (self);
  msg->generation = generation;
  msg->page       = page;
  msg->page_hits  = page_hits;       /* may be NULL */
  msg->finished   = FALSE;
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                   hit_message_apply, msg, hit_message_free);
}

static void
post_finished (FwSearch *self, guint generation)
{
  HitMessage *msg = g_new0 (HitMessage, 1);
  msg->search     = g_object_ref (self);
  msg->generation = generation;
  msg->page       = -1;
  msg->page_hits  = NULL;
  msg->finished   = TRUE;
  self->finish_idle_id = g_idle_add_full (
    G_PRIORITY_DEFAULT_IDLE, hit_message_apply, msg, hit_message_free);
}

typedef struct {
  FwSearch    *search;          /* strong ref */
  FwDocument  *document;        /* strong ref */
  char        *query;           /* owned copy */
  int          start_page;
  guint        generation;
} WorkerCtx;

static gpointer
search_worker (gpointer data)
{
  WorkerCtx  *ctx  = data;
  FwSearch   *self = ctx->search;
  FwDocument *doc  = ctx->document;

  int total = fw_document_get_page_count (doc);
  if (total <= 0)
    goto out;

  int start = ctx->start_page;
  if (start < 0)         start = 0;
  if (start >= total)    start = 0;

  FW_TRACE_VIEW ("search worker: query='%s' start=%d total=%d gen=%u",
                  ctx->query, start, total, ctx->generation);

  /* Scan from start_page → end, then wrap from 0 → start_page. Matches
   * near where the reader is reading appear first.
   *
   * Cancellation: each iteration compares `ctx->generation` (captured
   * at worker start) against the live `self->generation`. A new
   * find() / clear() / set_document() bumps the live counter, so this
   * worker recognizes itself as superseded and bails. Replaces the
   * earlier `cancel` flag + main-thread g_thread_join pattern, which
   * had the side effect of stalling the UI on a slow DjVu first
   * page. */
  for (int i = 0; i < total; i++) {
    if (g_atomic_int_get ((const gint *) &self->generation) != (gint) ctx->generation)
      break;

    int page = (start + i) % total;
    GArray *hits = fw_document_search (doc, ctx->query, page);
    /* Even an empty hits array is meaningful as a progress beat — but we
     * only post when there's something, to keep the idle queue light. */
    if (hits && hits->len > 0)
      post_hits (self, ctx->generation, page, hits);
    else if (hits)
      g_array_unref (hits);
  }

out:
  if (g_atomic_int_get ((const gint *) &self->generation) == (gint) ctx->generation)
    post_finished (self, ctx->generation);

  g_free (ctx->query);
  g_object_unref (ctx->document);
  g_object_unref (ctx->search);
  g_free (ctx);
  return NULL;
}

/* ── Public ─────────────────────────────────────────────────────────── */

FwSearch *
fw_search_new (void)
{
  return g_object_new (FW_TYPE_SEARCH, NULL);
}

static void
stop_worker_locked (FwSearch *self)
{
  /* Detach (don't join) — worker observes the bumped generation on its
   * next iteration and exits on its own. Joining the worker on the
   * main thread would stall the UI for one full page-search worth of
   * work, which on a slow DjVu first page is hundreds of milliseconds.
   * Each WorkerCtx holds strong refs to self + document, so the
   * worker can outlive any caller-side change without UAF. */
  if (self->worker) {
    g_thread_unref (self->worker);
    self->worker = NULL;
  }
}

void
fw_search_set_document (FwSearch *self, FwDocument *document)
{
  g_return_if_fail (FW_IS_SEARCH (self));

  stop_worker_locked (self);
  self->generation++;
  if (self->finish_idle_id) {
    g_source_remove (self->finish_idle_id);
    self->finish_idle_id = 0;
  }

  g_set_object (&self->document, document);
  g_array_set_size (self->hits, 0);
  self->current = -1;
  g_clear_pointer (&self->query, g_free);
}

void
fw_search_clear (FwSearch *self)
{
  g_return_if_fail (FW_IS_SEARCH (self));

  stop_worker_locked (self);
  self->generation++;
  if (self->finish_idle_id) {
    g_source_remove (self->finish_idle_id);
    self->finish_idle_id = 0;
  }

  gboolean had_hits = self->hits->len > 0;
  g_array_set_size (self->hits, 0);
  self->current = -1;
  g_clear_pointer (&self->query, g_free);

  if (had_hits) {
    g_signal_emit (self, signals[SIGNAL_HITS_CHANGED], 0);
    g_signal_emit (self, signals[SIGNAL_CURRENT_CHANGED], 0);
  }
}

void
fw_search_find (FwSearch *self, const char *text, int start_page)
{
  g_return_if_fail (FW_IS_SEARCH (self));

  /* Stop any in-flight scan and reset state. */
  stop_worker_locked (self);
  self->generation++;
  if (self->finish_idle_id) {
    g_source_remove (self->finish_idle_id);
    self->finish_idle_id = 0;
  }

  g_array_set_size (self->hits, 0);
  self->current = -1;
  g_clear_pointer (&self->query, g_free);
  g_signal_emit (self, signals[SIGNAL_HITS_CHANGED], 0);
  g_signal_emit (self, signals[SIGNAL_CURRENT_CHANGED], 0);

  if (!self->document || !text || !text[0])
    return;

  self->query = g_strdup (text);

  WorkerCtx *ctx = g_new0 (WorkerCtx, 1);
  ctx->search     = g_object_ref (self);
  ctx->document   = g_object_ref (self->document);
  ctx->query      = g_strdup (text);
  ctx->start_page = start_page;
  ctx->generation = self->generation;

  self->worker = g_thread_new ("fw-search", search_worker, ctx);
}

void
fw_search_next (FwSearch *self)
{
  g_return_if_fail (FW_IS_SEARCH (self));
  if (self->hits->len == 0)
    return;
  self->current = (self->current + 1) % (int) self->hits->len;
  g_signal_emit (self, signals[SIGNAL_CURRENT_CHANGED], 0);
}

void
fw_search_prev (FwSearch *self)
{
  g_return_if_fail (FW_IS_SEARCH (self));
  if (self->hits->len == 0)
    return;
  self->current--;
  if (self->current < 0)
    self->current = (int) self->hits->len - 1;
  g_signal_emit (self, signals[SIGNAL_CURRENT_CHANGED], 0);
}

int
fw_search_get_count (FwSearch *self)
{
  g_return_val_if_fail (FW_IS_SEARCH (self), 0);
  return (int) self->hits->len;
}

int
fw_search_get_current (FwSearch *self)
{
  g_return_val_if_fail (FW_IS_SEARCH (self), -1);
  return self->current;
}

gboolean
fw_search_is_running (FwSearch *self)
{
  g_return_val_if_fail (FW_IS_SEARCH (self), FALSE);
  return self->worker != NULL;
}

int
fw_search_get_current_page (FwSearch *self)
{
  g_return_val_if_fail (FW_IS_SEARCH (self), -1);
  if (self->current < 0 || (guint) self->current >= self->hits->len)
    return -1;
  return g_array_index (self->hits, FwSearchHit, self->current).page;
}

const FwSearchHit *
fw_search_peek_hits (FwSearch *self, int *out_count)
{
  g_return_val_if_fail (FW_IS_SEARCH (self), NULL);
  if (out_count) *out_count = (int) self->hits->len;
  return self->hits->len ? (const FwSearchHit *) self->hits->data : NULL;
}

int
fw_search_active_index (FwSearch *self)
{
  g_return_val_if_fail (FW_IS_SEARCH (self), -1);
  return self->current;
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_search_dispose (GObject *object)
{
  FwSearch *self = FW_SEARCH (object);

  stop_worker_locked (self);
  if (self->finish_idle_id) {
    g_source_remove (self->finish_idle_id);
    self->finish_idle_id = 0;
  }
  g_clear_object (&self->document);
  G_OBJECT_CLASS (fw_search_parent_class)->dispose (object);
}

static void
fw_search_finalize (GObject *object)
{
  FwSearch *self = FW_SEARCH (object);
  g_clear_pointer (&self->query, g_free);
  if (self->hits)
    g_array_unref (self->hits);
  G_OBJECT_CLASS (fw_search_parent_class)->finalize (object);
}

static void
fw_search_class_init (FwSearchClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose  = fw_search_dispose;
  G_OBJECT_CLASS (klass)->finalize = fw_search_finalize;

  signals[SIGNAL_HITS_CHANGED] = g_signal_new (
    "hits-changed", FW_TYPE_SEARCH,
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_CURRENT_CHANGED] = g_signal_new (
    "current-changed", FW_TYPE_SEARCH,
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_SEARCH_FINISHED] = g_signal_new (
    "search-finished", FW_TYPE_SEARCH,
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void
fw_search_init (FwSearch *self)
{
  self->current = -1;
  self->hits = g_array_new (FALSE, FALSE, sizeof (FwSearchHit));
}
