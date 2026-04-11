/* fw-cache.c — Pre-cache engine implementation
 *
 * Renders document pages asynchronously using a thread pool.
 * Pages are stored as cairo_surface_t* in a hash table keyed by page number.
 * Priority rendering: visible pages first, then forward, then backward.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fw-cache.h"

typedef struct {
  cairo_surface_t *surface;    /* NULL if not yet rendered */
  gboolean         rendering;  /* TRUE if a job is in the pool for this page */
  guint            generation; /* generation when this surface was rendered */
} CacheEntry;

typedef struct {
  FwCache *cache;   /* weak — job checks if cache is still alive */
  int      page;
  double   zoom;
  int      rotation;
  guint    generation; /* invalidation counter at time of job creation */
} RenderJob;

typedef enum {
  FW_RENDER_STATE_STATIC,
  FW_RENDER_STATE_CRUISING,
  FW_RENDER_STATE_SCRUBBING
} FwRenderState;

struct _FwCache {
  GObject        parent_instance;

  FwDocument    *document;    /* owned reference */
  GHashTable    *pages;       /* int → CacheEntry* */
  GThreadPool   *pool;
  GMutex         lock;

  double         zoom;
  int            rotation;
  int            page_count;
  guint          generation;  /* bumped on invalidate to cancel stale jobs */

  /* Velocity and queuing */
  double         velocity;
  FwRenderState  render_state;
  int            active_jobs;
  int            max_jobs;

  /* Priority state */
  int           *priority_order;  /* page indices in render priority order */
  int            priority_len;

  GtkWidget     *view_widget;  /* weak ref for scheduling redraws */
};

G_DEFINE_FINAL_TYPE (FwCache, fw_cache, G_TYPE_OBJECT)

/* ── Helpers ──────────────────────────────────────────────────────── */

static void
cache_entry_free (CacheEntry *entry)
{
  if (entry->surface)
    cairo_surface_destroy (entry->surface);
  g_free (entry);
}

static CacheEntry *
get_or_create_entry (FwCache *self, int page)
{
  CacheEntry *entry = g_hash_table_lookup (self->pages,
                                            GINT_TO_POINTER (page));
  if (!entry) {
    entry = g_new0 (CacheEntry, 1);
    g_hash_table_insert (self->pages, GINT_TO_POINTER (page), entry);
  }
  return entry;
}

/* Max pages to keep in memory: visible + buffer ahead/behind.
 * At fit-width on 1920px, a typical page surface is ~3-6 MB.
 * 50 pages ≈ 150-300 MB — comfortable on 16 GB RAM. */
#define CACHE_WINDOW 50

/* ── Thread pool worker ───────────────────────────────────────────── */

static void submit_next_jobs (FwCache *self);

static void
render_worker (gpointer data, gpointer user_data)
{
  (void) user_data;
  RenderJob *job = data;
  FwCache *self = job->cache;

  /* Check if this job is still relevant */
  g_mutex_lock (&self->lock);
  gboolean stale = (job->generation != self->generation);
  gboolean scrubbing = (self->render_state == FW_RENDER_STATE_SCRUBBING);
  if (stale || scrubbing) {
    if (!stale && scrubbing) {
      CacheEntry *entry = g_hash_table_lookup (self->pages, GINT_TO_POINTER (job->page));
      if (entry) entry->rendering = FALSE;
    }
    self->active_jobs--;
    submit_next_jobs (self);
    g_mutex_unlock (&self->lock);
    g_free (job);
    return;
  }
  g_mutex_unlock (&self->lock);

  /* Render the page (this is the expensive part — runs unlocked) */
  cairo_surface_t *surface =
    fw_document_render_page (self->document, job->page,
                             job->zoom, job->rotation);

  /* Store result */
  g_mutex_lock (&self->lock);
  if (job->generation == self->generation) {
    CacheEntry *entry = get_or_create_entry (self, job->page);
    if (entry->surface)
      cairo_surface_destroy (entry->surface);
    entry->surface    = surface;
    entry->rendering  = FALSE;
    entry->generation = job->generation;

    /* Schedule a redraw on the main thread so the new surface appears */
    if (self->view_widget)
      g_idle_add_once ((GSourceOnceFunc) gtk_widget_queue_draw,
                       self->view_widget);
  } else {
    /* Stale — discard */
    if (surface)
      cairo_surface_destroy (surface);
  }
  
  self->active_jobs--;
  submit_next_jobs (self);
  g_mutex_unlock (&self->lock);

  g_free (job);
}

static void
submit_next_jobs (FwCache *self)
{
  if (self->render_state == FW_RENDER_STATE_SCRUBBING)
    return;

  while (self->active_jobs < self->max_jobs) {
    int page_to_submit = -1;
    for (int i = 0; i < self->priority_len; i++) {
      int pg = self->priority_order[i];
      CacheEntry *entry = get_or_create_entry (self, pg);
      if (!entry->rendering && (!entry->surface || entry->generation != self->generation)) {
        page_to_submit = pg;
        break;
      }
    }

    if (page_to_submit == -1)
      break;

    CacheEntry *entry = get_or_create_entry (self, page_to_submit);
    entry->rendering = TRUE;
    self->active_jobs++;

    RenderJob *job = g_new0 (RenderJob, 1);
    job->cache      = self;
    job->page       = page_to_submit;
    job->zoom       = self->zoom;
    job->rotation   = self->rotation;
    job->generation = self->generation;

    g_thread_pool_push (self->pool, job, NULL);
  }
}

/* ── Public API ───────────────────────────────────────────────────── */

FwCache *
fw_cache_new (FwDocument *document, GtkWidget *view_widget)
{
  g_return_val_if_fail (FW_IS_DOCUMENT (document), NULL);

  FwCache *self = g_object_new (FW_TYPE_CACHE, NULL);
  self->document    = g_object_ref (document);
  self->page_count  = fw_document_get_page_count (document);
  self->view_widget = view_widget;  /* weak ref — outlives cache */
  return self;
}

void
fw_cache_start (FwCache *self, double zoom, int rotation)
{
  g_return_if_fail (FW_IS_CACHE (self));

  g_mutex_lock (&self->lock);

  self->zoom     = zoom;
  self->rotation = rotation;
  self->generation++;

  /* Do NOT clear existing cache — keep old surfaces visible while new
   * ones render. Mark all entries as needing re-render. */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, self->pages);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    CacheEntry *entry = value;
    entry->rendering = FALSE;  /* allow re-submission */
  }

  /* Submit only pages in the priority window.  If no priority has been
   * set yet (first open), submit the first few pages so there
   * is something to show immediately. */
  if (self->priority_order && self->priority_len > 0) {
    submit_next_jobs (self);
  } else {
    int n = self->page_count < 14 ? self->page_count : 14;
    self->priority_order = g_new (int, n);
    for (int i = 0; i < n; i++)
      self->priority_order[i] = i;
    self->priority_len = n;
    submit_next_jobs (self);
  }

  g_mutex_unlock (&self->lock);
}

void
fw_cache_invalidate_all (FwCache *self)
{
  g_return_if_fail (FW_IS_CACHE (self));

  g_mutex_lock (&self->lock);
  self->generation++;
  g_hash_table_remove_all (self->pages);
  g_mutex_unlock (&self->lock);
}

void
fw_cache_invalidate_page (FwCache *self, int page)
{
  g_return_if_fail (FW_IS_CACHE (self));

  g_mutex_lock (&self->lock);
  g_hash_table_remove (self->pages, GINT_TO_POINTER (page));
  g_mutex_unlock (&self->lock);
}

void
fw_cache_set_priority (FwCache *self, const int *visible_pages, int n_visible)
{
  g_return_if_fail (FW_IS_CACHE (self));

  g_mutex_lock (&self->lock);

  g_free (self->priority_order);

  if (n_visible <= 0 || !visible_pages) {
    self->priority_order = NULL;
    self->priority_len   = 0;
    g_mutex_unlock (&self->lock);
    return;
  }

  int first_visible = visible_pages[0];
  int last_visible  = visible_pages[n_visible - 1];
  int total = self->page_count;

  /* Build priority: visible, then ahead, then behind — but only
   * keep CACHE_WINDOW pages total. */
  int window = CACHE_WINDOW < total ? CACHE_WINDOW : total;
  self->priority_order = g_new (int, window);
  int idx = 0;

  /* 1. Visible pages */
  for (int i = 0; i < n_visible && idx < window; i++)
    self->priority_order[idx++] = visible_pages[i];

  /* 2. Pages ahead (after last visible) */
  for (int i = last_visible + 1; i < total && idx < window; i++)
    self->priority_order[idx++] = i;

  /* 3. Pages behind (before first visible), in reverse */
  for (int i = first_visible - 1; i >= 0 && idx < window; i--)
    self->priority_order[idx++] = i;

  self->priority_len = idx;

  /* Evict pages outside the window to free RAM — but only if they've
   * already been replaced at the current generation. Keep stale surfaces
   * so users see the old render instead of grey while re-rendering. */
  GHashTable *keep_set = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (int i = 0; i < self->priority_len; i++)
    g_hash_table_add (keep_set, GINT_TO_POINTER (self->priority_order[i]));

  GHashTableIter iter;
  gpointer key, value;
  GArray *to_remove = g_array_new (FALSE, FALSE, sizeof (int));
  g_hash_table_iter_init (&iter, self->pages);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    if (!g_hash_table_contains (keep_set, key)) {
      CacheEntry *entry = value;
      /* Only evict if the surface is current-gen (already replaced)
       * or if there's no surface at all */
      if (!entry->surface || entry->generation == self->generation) {
        int pg = GPOINTER_TO_INT (key);
        g_array_append_val (to_remove, pg);
      }
    }
  }
  for (guint i = 0; i < to_remove->len; i++)
    g_hash_table_remove (self->pages,
                          GINT_TO_POINTER (g_array_index (to_remove, int, i)));
  g_array_unref (to_remove);
  g_hash_table_unref (keep_set);

  /* Submit renders for pages in the window */
  submit_next_jobs (self);

  g_mutex_unlock (&self->lock);
}

gboolean
fw_cache_set_velocity (FwCache *self, double velocity)
{
  g_return_val_if_fail (FW_IS_CACHE (self), FALSE);

  g_mutex_lock (&self->lock);

  self->velocity = velocity;
  FwRenderState new_state;
  if (velocity > 2.0)
    new_state = FW_RENDER_STATE_SCRUBBING;
  else if (velocity > 0.2)
    new_state = FW_RENDER_STATE_CRUISING;
  else
    new_state = FW_RENDER_STATE_STATIC;

  gboolean state_changed = (self->render_state != new_state);

  if (state_changed) {
    self->render_state = new_state;

    if (new_state == FW_RENDER_STATE_SCRUBBING) {
      /* High-Velocity Abort: drop queued jobs instantly */
      self->generation++;
    } else {
      /* Submit jobs again if we dropped back to cruising or static */
      submit_next_jobs (self);
    }
  }

  g_mutex_unlock (&self->lock);
  return state_changed;
}

cairo_surface_t *
fw_cache_get_page (FwCache *self, int page)
{
  g_return_val_if_fail (FW_IS_CACHE (self), NULL);

  g_mutex_lock (&self->lock);
  CacheEntry *entry = g_hash_table_lookup (self->pages,
                                            GINT_TO_POINTER (page));
  cairo_surface_t *surface = NULL;
  if (entry && entry->surface)
    surface = cairo_surface_reference (entry->surface);
  g_mutex_unlock (&self->lock);

  return surface;
}

gboolean
fw_cache_page_ready (FwCache *self, int page)
{
  g_return_val_if_fail (FW_IS_CACHE (self), FALSE);

  g_mutex_lock (&self->lock);
  CacheEntry *entry = g_hash_table_lookup (self->pages,
                                            GINT_TO_POINTER (page));
  gboolean ready = (entry && entry->surface != NULL);
  g_mutex_unlock (&self->lock);

  return ready;
}

void
fw_cache_stop (FwCache *self)
{
  g_return_if_fail (FW_IS_CACHE (self));

  g_mutex_lock (&self->lock);
  self->generation++;  /* invalidate all pending jobs */
  g_mutex_unlock (&self->lock);
}

/* ── GObject boilerplate ──────────────────────────────────────────── */

static void
fw_cache_dispose (GObject *object)
{
  FwCache *self = FW_CACHE (object);

  fw_cache_stop (self);

  /* Wait for all pool threads to finish */
  if (self->pool) {
    g_thread_pool_free (self->pool, TRUE, TRUE);
    self->pool = NULL;
  }

  g_clear_object (&self->document);

  G_OBJECT_CLASS (fw_cache_parent_class)->dispose (object);
}

static void
fw_cache_finalize (GObject *object)
{
  FwCache *self = FW_CACHE (object);

  g_hash_table_unref (self->pages);
  g_mutex_clear (&self->lock);
  g_free (self->priority_order);

  G_OBJECT_CLASS (fw_cache_parent_class)->finalize (object);
}

static void
fw_cache_class_init (FwCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose  = fw_cache_dispose;
  object_class->finalize = fw_cache_finalize;
}

static void
fw_cache_init (FwCache *self)
{
  g_mutex_init (&self->lock);

  self->pages = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                        NULL,
                                        (GDestroyNotify) cache_entry_free);

  /* Thread pool: use number of processors, non-exclusive so threads
   * are shared across pools. */
  int n_threads = (int) g_get_num_processors ();
  if (n_threads < 2) n_threads = 2;
  if (n_threads > 8) n_threads = 8;

  self->pool = g_thread_pool_new (render_worker, NULL, n_threads, FALSE, NULL);
  self->max_jobs = n_threads;
  self->active_jobs = 0;
  self->render_state = FW_RENDER_STATE_STATIC;
}
