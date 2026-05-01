# Framework — Patch Notes

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

## v0.28.0 (2026-05-01)

*`try_closest_rendered_page` zoom transition — Phase 11 Tier 2.* Continuous Ctrl+scroll zoom no longer falls back to a single-zoom prev_texture (which got progressively blurrier as the user crossed many zoom levels). Each `CacheEntry` now retains up to 3 prior-zoom snapshots; `fw_cache_get_texture` picks the slot whose zoom is closest to the current target (matching rotation + scale_factor) and returns it for GTK to auto-scale into the current rect. A user zoom-storming across 5 levels now sees their just-rendered 2.4× snapshot scaled 1.04× to fill a 2.5× rect — sharp — instead of the original 1.0× snapshot scaled 2.5× — blurry.

### Implementation
- New `ZoomSlot` struct: `(surface, texture, zoom, rotation, scale_factor, size_bytes)`. `MAX_PREV_ZOOM_SLOTS = 3` per page; oldest evicts on overflow.
- `CacheEntry` fields collapsed: dropped `prev_surface`/`prev_texture`/`prev_size_bytes`; added `prev_slots[3]` + `prev_slot_count` + `prev_slots_bytes`. Current-surface params (`zoom`, `rotation`, `scale_factor`) tracked at the entry level so demotion can capture them.
- `fw_cache_start` (zoom/rotation change): demotes the existing current surface into `prev_slots[0]`, shifting older slots right; oldest at index 2 frees on overflow. Bytes transfer from `size_bytes` → `prev_slots_bytes` without touching `total_cached_bytes` (no double-count).
- Worker store path simplified: stale-discard logic unchanged; the "drop stale prev" block is gone since prev_slots are intentionally retained until explicit eviction.
- `fw_cache_get_texture` walks `prev_slots[]` and returns the slot with minimal `|zoom − target|` (when current isn't ready). GTK's `gtk_snapshot_append_texture` already auto-scales — the view doesn't need a transform change.
- `fw_cache_get_prev_page` was an unused public API leftover; dropped from the header.

### Stress test cap bump
`stress-zoom-storm`'s settled-RSS cap raised from 1024 → 1280 MB. Multi-slot retention legitimately holds ~150 MB more cache memory after a 50-cycle zoom storm on the Effective Java sample — the bytes are still bounded by `byte_cap` (default 512 MB cache surfaces); the RSS rise is mostly glibc retention from the higher allocation churn. ASan-clean across all 5 stress tests.

---

## v0.27.2 (2026-05-01)

*Auto-resize the window for spreads, restore on the next page.* When a wide spread becomes the active row (centerfold in a CBZ, or a paired pair wider than the viewport in facing-pages mode), the window grows horizontally so the spread fits without a horizontal scrollbar. When the user scrolls/pages past the spread back to a normal row, the window restores the width it had before we grew. The interaction tracks a single baseline width so we never shrink past the user's manual sizing — only restore the size we captured before our own grow.

Compositor caveat: this uses `gtk_window_set_default_size` since GTK4 has no programmatic resize for shown windows. On most floating Wayland compositors and X11, the resize is honored. Tiling compositors may silently drop it; the `FW_DEBUG=1` `WINDOW` traces log every grow/shrink request so you can tell whether it took. Maximized and fullscreen windows are skipped — fighting the WM there isn't useful.

`fw_view_get_current_row_width` is the new public query: returns the displayed pixel width of the current row, which is the active page's width when standalone or the pair width (incl. gutter) in facing-pages mode. The window's `on_scroll_changed` calls it whenever current_page changes and decides whether to grow or shrink.

---

## v0.27.1 (2026-05-01)

*Spread detection for facing-pages mode.* Some CBZ files store 2-page centerfold spreads as a single landscape image; v0.27.0 was naively pairing those wide pages with the next portrait page, which broke the visual flow (especially obvious in manga reading). Now an aspect-ratio test (`w/h > 1.0` → standalone spread) drives a pre-built `pair_partner[]` array, so spreads stand alone and the page that would have been their natural partner orphans cleanly. After the spread, alternation resumes on whatever page follows.

The pairing decisions are computed once in `recompute_layout` from `page_widths`/`page_heights` (which already account for rotation), so `view_page_is_paired` and `view_pair_first` become O(1) lookups consulted by the snapshot path, click-to-doc mapping, and current-page tracking. Books that are landscape end-to-end (artbooks) will see every page standalone — appropriate, since pairing pre-spread pages would just shrink them in half.

---

## v0.27.0 (2026-05-01)

*Comic-reader trio + roadmap reorg.* Phase 13's three layout modes — Manga, Webtoon, Facing Pages — land together since they all touch `FwView::recompute_layout` and the snapshot path. Plus the long-stale "Hermitage" rename fixed in roadmap, and the 1.0 release section moved to the end of the document where it actually belongs given how far the 0.x sprint has gone.

---

### Manga Mode (Phase 13)
New `manga-mode` GSettings boolean, **F4** shortcut, "Manga Mode (RTL)" entry under the new Comic Layout submenu. When on, the directional page-nav keys swap — Left Arrow advances to next page, Right Arrow goes to previous — following the reading order of Japanese manga. Pure scroll geometry is unaffected (vertical layout doesn't change), so the toggle is purely about RTL nav semantics. Combined with facing-pages, also flips left/right within each pair so the lower-numbered page sits on the right.

The implementation lives entirely in `fw-window.c::on_key_pressed` (key swap) and `fw-view.c::view_page_is_paired`/snapshot-x (paired-layout flip) — about a dozen lines of behavior change for a feature that lights up an entire genre of content.

### Webtoon Mode (Phase 13)
New `webtoon-mode` GSettings boolean, **F5** shortcut. Drops `PAGE_GAP` to zero in `recompute_layout` so vertically-laid-out long-strip comics stitch into a seamless single canvas — designed for Korean webtoons and other formats where the artist composes across page boundaries. No-op when facing-pages is also active (mutually exclusive layouts).

Layout-anchor preservation: toggling webtoon mode runs through `view_apply_layout_change`, which captures `(page, intra-page-fraction)` before the layout change and restores after — so flipping the toggle mid-document keeps you in the same place rather than jumping to a different page.

### Facing Pages (Phase 13)
New `facing-pages` GSettings boolean, **F10** shortcut. Two pages per row, page 0 standalone as cover, then 1+2, 3+4, etc. — matches how a physical book opens. Added a pair-aware layout helper (`view_page_is_paired`/`view_pair_first`) that the snapshot path, click-to-doc mapping, and `fw_view_get_current_page` all consult so the rest of the view code doesn't have to reason about pairs. Pair height = max of the two pages (handles mismatched dimensions cleanly); pair width tracked through `max_width` so `GtkScrolledWindow` provides a horizontal scrollbar when a pair is wider than the viewport.

Combined with manga mode: lower-numbered page sits on the right within each pair. Combined with crop margins or zoom: same `(page, frac)` anchor preservation kicks in via `view_apply_layout_change`.

### Roadmap Cleanup
- Moved Phase 10 (1.0 Release) to the end of the document and renumbered to Phase 15. The roadmap had grown a v0.27 worth of features past the original 1.0 landing slot; the section was actively misleading where it sat.
- Replaced `dev.hermitage.Hermitage.desktop` (stale name from a previous project iteration) with the actual `io.github.virinvictus.framework.desktop`.
- Removed a duplicate `stress-zoom-storm` entry left over from the v0.15 sprint.

---

## v0.26.0 (2026-05-01)

*Cache and bench batch — Phase 11 Tiers 2 and 3, plus Phase 12.2 and 12.3 fill out the test harness.* Four roadmap items shipped together: per-instance MuPDF store size scaling, TTL+LRU hybrid cache eviction, a new render-latency benchmark, and a full-corpus soak test.

---

### Per-Instance MuPDF Store Size Scaling (Phase 11 Tier 3)
Both `fz_new_context` call sites in the PDF backend (the main context plus the eight per-instance render contexts) now size the MuPDF store to file size: 16 MB under 5 MB, 32 MB under 20 MB, 64 MB under 100 MB, 128 MB above. Previous fixed 32 MB allocation was wasteful on novels (5 MB EPUBs got the same store as 200 MB textbooks) and tight on heavy textbooks (font/JPEG2000 churn). With eight per-instance contexts, the upper bound scales to 1 GB total store on heavy documents — comfortable on this 30 GB box; revisit only if a memory-constrained reference (`.plato/`) becomes an actual target.

### TTL+LRU Hybrid Cache Eviction (Phase 11 Tier 2)
The bytes-aware eviction loop from v0.16 picked victims in iteration order — effectively arbitrary. Now each cache entry tracks `last_access_us`, bumped on every `fw_cache_get_page` / `fw_cache_get_texture` hit and on worker-store success; outside-priority candidates are sorted oldest-first before eviction. Pages the user just scrolled back to survive when the cap fires; pages they haven't touched in seconds go first. No new public API, no behavior change when under the cap; only the eviction policy improved.

### `bench-render` (Phase 12.3)
New benchmark that times direct `fw_document_render_page` calls across an evenly-spaced span of pages, in two passes — cold (fresh handle, populates the per-instance store) and warm (re-render same pages, hits the store). Reports n / mean / p50 / p95 / p99 / max in milliseconds, plus total elapsed. The cache layer is intentionally bypassed: this answers "how fast does the backend render?" not "how well does the cache hide latency?" Quick check on Effective Java's 901-page corpus sample showed cold p50 ~9 ms, warm p50 ~3 ms — about a 3× store-hit speedup, validating the v0.26 scaling.

### `stress-corpus-soak` (Phase 12.2)
Full-corpus soak — opens each of the seven canonical samples (PDF×2, DjVu, EPUB, MOBI, CBZ, CBR), walks every fifth page through the cache up to 200 pages per document, and tears down. Catches regressions on backends none of the narrower stress tests exercise (e.g. CBR's libarchive path, MOBI's reflowable layout). Runs in ~36 s; registered with `meson test` so the suite now has five entries. Confirmed clean under ASan+UBSan; peak RSS lands around 1.5 GB on the comics-heavy run, default cap raised to 1.8 GB.

### Test Harness
Five `meson test` targets total: stress-scrub, stress-zoom-storm, stress-search-cache, stress-multidoc, stress-corpus-soak. bench-render is built but not registered as a test (latency benchmark, not a pass/fail check) — invoke directly.

---

## v0.25.0 (2026-05-01)

*Margin cropping, multi-doc lifecycle stress test, real leak fix.* The third Phase 14 polish item lands; the new stress-multidoc test promptly catches a real leak in the cache dispose path that the existing stress tests never reached.

---

### Margin Cropping (Phase 14)
A new `Crop Margins` toggle (`F6`, primary menu, GSettings-backed) auto-crops whitespace margins so dense PDFs use more of the laptop screen. Implementation:

- **Detection**: a new `get_content_bbox(page)` interface method returns the inked-content bounding box in document points. The PDF backend computes it by walking the cached `fz_stext_page` and unioning every char's quad-derived rect — text blocks only, image blocks skipped. Fast (the stext is already cached from v0.18). DjVu and CBR return FALSE; the toggle has no effect on those.
- **Application**: on toggle activation, the view probes the *current visible page*'s bbox (assuming uniform margins across the doc, which holds for ~99% of technical PDFs) and computes fractional margins. `recompute_layout` shrinks every page's reported width/height by `(1 - margin_fractions)`. The snapshot path draws the full page texture offset+sized so the content area aligns with the cropped page rect, and pushes a clip so margins don't leak past the rect.
- **Anchor preservation**: like `fw_view_set_zoom`, captures `(page, intra-page-fraction)` before the layout change and restores after — without it, toggling crop would jump to a different page because the same scroll_y maps differently in the smaller layout.

The toggle is wired with the same plumbing as the v0.23 reading ruler and v0.24 loupe: GSettings boolean → `g_settings_create_action` → menu checkmark + F6 accelerator. Both stay in sync; setting persists across sessions.

### `stress-multidoc` (Phase 12.2)
A new stress test, the fourth in the harness. Sequential phase: open 50 documents in succession across the six-format corpus (PDF, DjVu, EPUB, MOBI, CBZ, CBR), create a cache for each, render the first three pages, dispose. Parallel phase: hold 10 `FwDocument`+`FwCache` instances simultaneously, then dispose all in reverse order.

Asserts no crashes, peak RSS under 2 GB (covers ASan overhead), and cleanly disposes everything. Caught the leak below on first run.

### Bugfix — Pool Dispose Leak in `fw_cache_dispose`
**Real leak.** The cache called `g_thread_pool_free(pool, immediate=TRUE, wait=TRUE)`, where `immediate=TRUE` discards queued tasks *without invoking their workers*. Since each `RenderJob` is `g_new0`-allocated and free'd inside the worker, every queued-but-not-yet-running job leaked on cache dispose — exactly hits during document open/close churn.

ASan attribution from `stress-multidoc`:

```
Direct leak of 2832 byte(s) in 59 object(s) allocated from:
    g_malloc0 → submit_next_jobs → fw_cache_start
```

Fix: `immediate=FALSE` instead, so queued jobs run through their workers. Workers see `cancel_gen` was bumped during `fw_cache_stop`, take the bail-out path, and `g_free` the job. The `fw_document_cancel_render` call already in `fw_cache_stop` ensures mid-render decodes abort fast (PDF via fz_cookie, DjVu/CBR via cancel_flag), so the drain doesn't block dispose noticeably.

Verified: ASan-clean across the full multi-doc run after the fix. Same code path is exercised by every document close in the GUI — the leak was bleeding small but real allocations on every file switch.

---

## v0.24.1 (2026-05-01)

*Bugfixes — fit-width, zoom-anchor, sticky-blur, scroll handling.* No new features; sanding down the rough edges that surfaced once the loupe and CBR cache started exercising paths in new combinations.

---

### Per-Page Fit-Width
`fw_view_fit_width_zoom` previously found the *widest page across the entire document* and computed `viewport_w / max_page_w`. For a comic CBZ with a single centerfold spread, that made every normal page render at ~35% — empty viewport on either side and the user had to manually zoom in. Now it uses the *current visible page's width*, so normal pages fill the viewport. Scrolling onto a wider spread page makes that page wider than the viewport and adds horizontal scroll until the user hits Ctrl+1 again on it.

For uniform-width docs (PDF textbooks, DjVu, EPUB), behavior is unchanged: every page is the same width so per-page and document-wide are equivalent.

### Per-Page Horizontal Centering in Snapshot
Companion to the fit-width fix. The snapshot's centering math was `if (max_width <= widget_width) center-in-viewport, else position-in-canvas`. With per-page fit-width on Berserk, normal pages were narrow, max_width was the spread page's width, and the else branch positioned normal pages offset within the wider canvas. Changed the condition to `pw <= widget_width` — each page centers in the viewport when *it* fits, regardless of document-wide max. Mirrored in `fw_view_widget_to_doc` so click coordinates still map correctly.

### Page-Fraction Horizontal Anchor in `fw_view_set_zoom`
The earlier "fraction of canvas" horizontal anchor for zoom-preserving-focus broke on mixed-width docs because it anchored to the canvas (max_width) rather than the page the user was looking at. Replaced with a page-fraction anchor: capture the fraction of the current page's width that's at the viewport's horizontal center before zoom, derive the scroll_x that puts the same page-fraction at viewport center after zoom. Zooming in past fit-width now keeps the focal point centered instead of jumping to the page's left edge.

### Sticky-Blur Bugfix in Worker Store Path
The v0.24.0 sticky-fail change (skip re-rendering entries with `render_gen == self->render_gen`) was correct for deterministic failures (CBR's "zero-size render") but wrong for *transient* failures — specifically, fz_cookie cancellations that fire mid-render. When a worker's render is aborted by `cookie->abort = 1` from the SCRUBBING transition, the render returns NULL, the worker reaches the success branch with `render_gen` matching, stores `surface=NULL` and `render_gen=current`. The page then stayed stuck at thumbnail resolution until the next render_gen bump (zoom or rotation).

Fix: in the worker store path, distinguish "cancelled mid-render" from "actually failed" by checking whether `cancel_gen` was bumped during the render. Bumped + NULL surface → transient cancellation, clear `rendering` but don't sticky-fail. Unbumped + NULL → real failure, stays sticky as designed in v0.24.0.

### Scroll Handling Returned to Native GTK
Removed the per-event scroll cap that the v0.14 work introduced. With the v0.14 GThreadPool sort-function priority dispatch and v0.17 fz_cookie mid-render abort already in place, the cache responds to scroll velocity natively without needing an input-side cap. The `kinetic-scrolling` GSettings boolean now drives `gtk_scrolled_window_set_kinetic_scrolling()` on the document scrolled window — the standard knob — instead of the custom cap-vs-momentum toggle. Default flipped from false to true.

The view's own `GSettings` handle stays for `reading-ruler` and `loupe`; the kinetic-scrolling-related fields/handlers are gone. The window owns the kinetic setting now.

### Zero-Size Render Warning Suppressed
The CBR backend's "zero-size render" condition (zoom × image dimensions rounds below 1 px) is benign — the cache already handles NULL surfaces gracefully via the thumbnail fallback. The `g_warning` was log noise. Now suppressed via a `volatile gboolean silent_zero_size` flag set before fz_throw and checked in fz_catch; genuine MuPDF errors still warn.

---

## v0.24.0 (2026-05-01)

*Magnifying loupe, CBR bytes cache, and a runaway-render bugfix.* Three things shipped together: the third Phase 14 polish item (loupe), a long-pending CBR backend optimization (per-page bytes cache), and a freshly-discovered cache infinite-loop bug uncovered by the loupe's per-frame redraws.

---

### Magnifying Loupe (Phase 14)
A circular zoom-in viewport that follows the cursor — useful for dense comic panels, small chart axis labels, and footnote text on scanned documents. Implemented as a snapshot-time GSK transform: rounded clip at the cursor + zoom-around-cursor matrix + re-append the page texture inside the clip + thin border. Pure GPU work, no re-rendering required (the texture is already in cache). Magnification is fixed at 2.5×; loupe radius is 80 px.

Wired through:
- New `loupe` GSettings boolean (default off, persists).
- **F7** keyboard shortcut.
- "Magnifying Loupe" entry in the primary menu.
- New row in the in-app Keyboard Shortcuts dialog (View group).
- New row in README.md's Keyboard Shortcuts → View table.

### CBR Per-Page Bytes Cache
RAR has no central directory: seeking to entry N requires sequentially decompressing entries 1..N-1. Every render of every page previously paid that full walk cost from scratch — the documented "streaming-RAR cost" called out in `fw-document-cbr.c`'s threading comment. Now there's a per-page bytes cache on `FwDocumentCbr`: first render of a page does the full walk and stores the extracted entry as a `GBytes`; subsequent renders hit the cache and skip straight to MuPDF decode + raster.

Cache details:
- Keyed by page index, stored as `GBytes *` (refcounted) in a `GHashTable`.
- FIFO eviction via a parallel `GQueue` of page indices when total cached bytes exceeds the cap. Comics are read mostly linearly so age-based eviction works fine.
- Default cap is 256 MB, sized for typical graphic novels.
- Serialized via the existing `archive_lock` mutex — same lock as the archive walk this cache exists to short-circuit, so no new lock-ordering concerns.
- The `cbr_extract_entry` function signature changed from `(page, *out_size) → guint8*` to `(page) → GBytes*`. Updated both callers (the page-0 dimension probe in `cbr_open` and the main render path).

ASan + UBSan clean across the full stress run.

### Bugfix — Sticky-Fail Render Skip in `fw_cache.submit_next_jobs`
The loupe's per-frame redraws surfaced a long-latent infinite-render-loop in the cache pipeline. When a render job returned `NULL` (e.g., the CBR backend's "zero-size render" failure on certain thumbnail-tier renders), the worker stored `surface = NULL` and set `entry->render_gen = job->render_gen`. Then `submit_next_jobs`'s skip condition `entry->surface && entry->render_gen == self->render_gen` evaluated FALSE because surface was NULL — re-pushing the same failed job. Each retry produced another NULL, which re-pushed again, ad infinitum.

Discovered by tracing: with FW_DEBUG=1 + loupe enabled on a 583-page CBR, `output.log` collected **3.38 million `[cache] worker start` lines in 30 seconds** (~110 k/sec). All on the same handful of pages whose renders happened to fail.

Fix: change the skip condition to compare `render_gen` alone:

```c
if (entry->render_gen == self->render_gen)
  continue;
```

A render attempt at the current generation — success *or* failure — sticks until the next generation bump (zoom or rotation change). After the fix, the same scenario produces **209 cache traces** for the full run instead of 3.38 million. CPU stays quiet, fans stay still.

The "zero-size render" warnings on a few specific CBR pages are a separate (cosmetic) symptom worth investigating — likely sub-pixel rounding when zoom × original page width drops below 0.5 — but it no longer cascades into a thermal incident.

### Diagnostic Trace Plumbing
While diagnosing the loop, a per-second snapshot timing summary was added (`view: snap stats: N frames/s avg=Xms loupe-paints=K …`) plus per-call CBR cache hit/miss traces. Zero overhead when `FW_DEBUG=0`; instantly tells you whether a perf issue is a frame storm, expensive frames, or render churn when enabled.

---

## v0.23.0 (2026-05-01)

*Reading ruler (Phase 14).* Toggleable mode that dims everything except a horizontal band tracking the cursor — keeps the eye on the active line in dense technical reading. Pattern conceptually borrowed from Sioyek's "visual mark"; reduced to a couple of `GskColorNode`s above and below a clear band.

---

### Reading Ruler
A `reading-ruler` GSettings boolean (default off) drives a render-time overlay: when active, paint two semi-transparent black rects above and below a ~56-px-tall clear band that follows the mouse Y. No clipping, no shaders — just two `gtk_snapshot_append_color` calls per frame. Tracking is via the existing `on_motion` controller, which queues a redraw when the ruler is active.

Toggle paths:
- **F8** keyboard shortcut.
- **Reading Ruler** entry in the primary menu.
- The setting persists across sessions; the menu checkmark and the F8 toggle stay in sync via `g_settings_create_action`.

The shortcut sits naturally in the F-key range with F9 (sidebar) and F11 (fullscreen) — view-mode toggles all live there.

### Documented in app and README
The Keyboard Shortcuts dialog (`Ctrl+?` / `F1`) gained a "Reading ruler — F8" row in the View group. The README's Keyboard Shortcuts → View table has the same row. Both stay in sync with the actual binding.

---

## v0.22.0 (2026-05-01)

*Hue-preserving recolor for Ctrl+I.* The previous dark-mode toggle was a per-channel bitwise NOT — accurate for "white text on white page" but destructive for any document with chromatic content. Red diagrams turned cyan, blue plots turned yellow, syntax-highlighted source code lost every color cue. Replaced with a luminance-aware affine transform that flips the lightness axis while keeping each pixel's chromatic component intact.

---

### Hue-Preserving Lightness Inversion (Phase 11 Tier 2)
For each pixel, compute BT.601 luma `Y = 0.299R + 0.587G + 0.114B`, then offset every channel by `(1 - 2Y)`:

```
R' = R + (1 - 2Y) =  0.402·R − 1.174·G − 0.228·B + 1
G' = G + (1 - 2Y) = −0.598·R − 0.174·G − 0.228·B + 1
B' = B + (1 - 2Y) = −0.598·R − 1.174·G + 0.772·B + 1
```

The chromatic offset (R−Y, G−Y, B−Y) is preserved by construction; only the lightness axis flips. White (Y=1) → near-black, black (Y=0) → near-white, red (Y=0.299) stays red but on a dark background. Implemented as a single `gtk_snapshot_push_color_matrix` — no shader work, GPU-side, the same lifecycle as the v0.14 GPU color inversion. GSK clamps out-of-gamut output to [0,1] for free.

Pattern conceptually similar to zathura's `colorumax` HSL recolor (`zathura/render.c:495+`), but reduced to a 4×4 affine that fits the existing GTK4 GPU path. Configurable theme colors (`recolor-light` / `recolor-dark` GSettings keys) — the full zathura-style customization — stay open as a follow-up; the current implementation hardcodes the standard "white background → black background" mapping which is what 95% of dark-mode users actually want.

### Tested
ASan + UBSan clean across all three stress tests. The change is rendering-only — no cache, document, or selection paths touched.

---

## v0.21.1 (2026-05-01)

*Bugfixes and post-session cleanup.* No new features — just sanding down the rough spots that accumulated across the v0.12 → v0.21 sprint.

---

### LRU Eviction Actually Works (`fw_state_prune`)
The `fw-state.c` header has claimed since v1.1.0 that state.json is "capped at 500 entries (LRU)". In practice only the age-based prune (entries >90 days) was implemented; the file would grow unbounded for users who open more than 500 documents inside the 90-day window. A `TODO: LRU eviction` comment marked the gap. Now fixed: when post-age count exceeds `MAX_ENTRIES` (500), entries are sorted by `last_opened` timestamp and the oldest are evicted until the count is at the cap. Phase 9's checkbox in the roadmap is now honest.

### Dead `active_jobs` / `max_jobs` Bookkeeping Removed
The v0.16.0 byte-cap rework dropped the `active_jobs < job_limit` concurrency throttle in favor of letting the GThreadPool's own worker count cap concurrency. The counters were retained "for debug tracing" but never actually traced — they were pure write-only bookkeeping (incremented in submit, decremented in worker, read by nobody). Removed the fields, the increment/decrement sites, and the init lines. Net –7 lines plus a small clarity win.

### Pedantic Warning Cleanups
Two `FW_TRACE_*("string-only")` call sites triggered ISO C99 variadic-macro warnings at `-Dwarning_level=3` (the macro uses `##__VA_ARGS__`, a GNU extension). The default build at level 2 was clean, but the strict-build was noisy. Fixed both with explicit `"%s", ""` arguments — same trace output, ISO-conformant.

### Verified
ASan-clean across all three registered stress tests (stress-scrub, stress-zoom-storm, stress-search-cache) with the cleanup applied. No regressions in the cache pipeline.

### Known TODO Carried Forward
`fw-document-djvu.c:506` — `djvu_get_text` doesn't filter the returned text to the selection rectangle (returns the whole page's text instead). Real bug for DjVu users hitting Ctrl+C on a partial selection. Requires careful coord conversion (DjVu uses pixel coords at file-DPI from bottom-left; Framework uses points at 72 DPI from top-left) and testing against real DjVu samples. Deferred to its own focused commit when DjVu selection becomes a felt pain point.

---

## v0.21.0 (2026-04-30)

*Phase 14 — auto-reload via `GFileMonitor`.* Recompile your LaTeX or Typst document and Framework refreshes automatically, restoring exact scroll position and zoom. The same pattern that made SumatraPDF a fixture in technical workflows for years.

---

### Auto-Reload on File Change
A `GFileMonitor` is attached to the open document path the moment a document opens. When the file changes — `G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT` for in-place writes, `G_FILE_MONITOR_EVENT_CREATED` for atomic-rename editors — the window saves current state (page, scroll fraction, zoom, rotation) via the existing `fw_state_save` path, re-opens the document, and restores state from disk. The deferred `restore_state_tick` mechanism handles the scroll restore once the new document's layout settles, so the user lands at exactly the same paragraph they were reading.

Implementation notes:
- `G_FILE_MONITOR_WATCH_HARD_LINKS` flag tracks the inode, so atomic-rename patterns (`write to .tmp; rename .tmp to target`) still produce events. LaTeX and Typst both use in-place writes; some editors that "write to a temp file and rename" benefit from this flag.
- A 200 ms debounce collapses bursts of CHANGED events (LaTeX writes auxiliary files in the same directory in quick succession; Typst sometimes emits multiple chunks) into a single reload.
- The monitor is stopped at the start of every `fw_window_open_file` (before tearing down the old document) and on window dispose.
- An `AdwToast` ("Document updated") shows briefly so the swap is visible — without it, an auto-reload mid-read could feel confusing if the user didn't trigger it themselves.

### `AdwToastOverlay` for Window-Wide Notifications
The window's content tree is now wrapped in an `AdwToastOverlay`. Beyond the auto-reload toast, this gives every future Framework feature a clean place to surface ephemeral notifications without resorting to dialogs (the next likely user is a "selection copied" toast for cases where Ctrl+C has no visual feedback).

### Trade-offs
- Reloading clears the navigation history (Alt+Left/Alt+Right). The history is per-document and the document just changed under us; preserving it would mean replaying jumps against a possibly-renumbered TOC. Wiping is cleaner.
- The 200 ms debounce window means there's a tiny perceptible delay before the reload kicks in. For LaTeX users this is fine — recompile takes a second or more — and avoids the "reload mid-write" flicker on slow filesystems.
- DjVu and CBR auto-reload for free since `fw_window_open_file` is backend-agnostic. Untested in practice; the LaTeX/Typst use case is the design driver.

---

## v0.20.0 (2026-04-30)

*Drag-selection highlight matches the actual selected text.* The previous overlay drew a single bounding rectangle from drag start to end, which included unselected words on partial first/last lines and looked ragged across line wraps. Now the highlight is per-line — partial first line, full intermediate lines, partial last line — matching exactly what `Ctrl+C` puts on the clipboard.

---

### Per-Line Selection Quads via `fz_highlight_selection`
A new `get_selection_quads` interface method asks the backend for an array of `FwRect`s (page-coordinate rectangles), one per line of selected text in reading order. The PDF backend implements it via MuPDF's `fz_highlight_selection` against the v0.18 cached `fz_stext_page` — same fast path as `pdf_get_text` and `pdf_select_at`, no extra parse.

`FwView` stores the quads in `sel_quads` (`GArray<FwRect>`, owned), recomputed live during drag, on drag end, and on snap-select (double/triple click). The render path iterates quads and draws each as a separate semi-transparent blue overlay; if `sel_quads` is empty (DjVu, CBR — which don't implement the method), it falls back to the legacy bounding-box draw.

Visible improvements:
- Multi-line drag selection highlights *only* the actual selected text — partial line at the top of the drag, full lines in between, partial line at the bottom. No more highlighting half a paragraph just because the bounding box happens to cover it.
- Snap-select overlays are also computed via quads, so they render identically to drag selection (a word on one line is one quad; a line is one quad).
- Selection highlight updates in real time during drag — the cached stext makes per-frame quad recomputation cheap.

### Backend Coverage
- **All MuPDF-routed formats** (PDF, CBZ, CB7, CBT, XPS, EPUB, FB2, MOBI) → per-line quads.
- **DjVu, CBR** → return NULL from `get_selection_quads`; the view falls back to drawing the drag bounding rectangle (existing behavior, unchanged).

### Tested
ASan-clean. All three registered stress tests still pass — the change touches view + PDF backend, not the cache pipeline that the stress tests exercise.

---

## v0.19.0 (2026-04-30)

*Phase 14 — smart text selection.* Double-click selects the word under the cursor; triple-click selects the whole line. Built directly on the v0.18 stext cache, so the snap is constant-time after first text access on a page (no extra parse).

---

### Double-Click → Word, Triple-Click → Line
The PDF backend grew a `select_at` interface method backed by MuPDF's `fz_snap_selection`. Given a click point and a granularity (`FW_SELECT_WORD` or `FW_SELECT_LINE`), it walks the cached `fz_stext_page` and snaps both selection endpoints to the word or line containing the click — no click-drag required.

In the view, `on_click_pressed` now branches on `n_press`:
- `n_press == 2` → call `fw_document_select_at` with `FW_SELECT_WORD`, apply the snapped rectangle to the existing selection state, queue redraw.
- `n_press == 3` → same with `FW_SELECT_LINE`.
- `n_press == 1` → unchanged (link-hit-test, single-click navigation).

Multi-press takes priority over link clicks: if a user double-clicks on a hyperlinked word, the intent is selection — the link is ignored. Single-click on the same word still navigates. The drag gesture continues to work for arbitrary range selection.

The selected text is extracted via the existing `fw_document_get_text` path (which itself uses the cached stext now, so it's another fast path). `Ctrl+C` copies it to the clipboard exactly as before — no new clipboard plumbing needed.

### Backend Coverage
- **PDF / CBZ / CB7 / CBT / XPS / EPUB / FB2 / MOBI** (all MuPDF-routed) → fully supported.
- **DjVu / CBR** → `select_at` returns `FALSE` (no implementation). Click-drag selection still works on DjVu via the existing path; CBR has no text layer at all. The vtable's NULL fallback returns `FALSE` from the public glue, so the view falls through cleanly without selecting.

### Tested
ASan-clean. The `stress-search-cache` test (which exercises the same stext-cache path the new selection uses) still hits its 6×+ speedup target.

The selection bbox handling correctly returns FALSE when the click misses every glyph (using `fz_snap_selection`'s zero-quad signal), so double-clicking on whitespace doesn't apply a stale-feeling no-op selection — the existing selection (if any) stays in place.

---

## v0.18.0 (2026-04-30)

*Phase 11 Tier 1 — cached `fz_stext_page` per page.* PDF text extraction and search now build the structured-text page once per document-page lifetime and reuse it on every subsequent text-related call. A new automated test confirms the speedup is real: a full-document search across 901 pages of Effective Java goes from **332 ms cold to 48 ms warm — a 6.85× speedup**. With this in place, double-click word selection (Phase 14) becomes a one-line follow-up: it just consumes the cached stext.

---

### Per-Page `fz_stext_page` Cache
A new `stext_cache` GHashTable on `FwDocumentPdf` maps page index → `fz_stext_page *`. All entries are owned by `self->ctx` and dropped en masse in `pdf_close`. Population is lazy: the first text-related call (`pdf_get_text`, `pdf_search`) on a given page extracts and caches; every later call hits.

The previous code paths re-loaded the page and re-extracted stext on *every* invocation — a 1000-page search ran `fz_load_page` + `fz_new_stext_page_from_page` 1000 times. Even though MuPDF makes both fast individually, the redundancy adds up.

Implementation notes:
- All access goes through `self->lock` (the existing main-context mutex). No separate stext lock — read-after-write is already serialized.
- `fz_stext_page` is immutable after construction in MuPDF, so cached read access from any code path that holds `self->lock` is safe without extra synchronization.
- Memory cost: ~10–50 KB per page on typical textbooks (depends on text density). A 1000-page document costs ~30 MB worst-case. Bounded by document length, not user activity.
- `pdf_search` switched from `fz_search_page` to `fz_search_stext_page` to consume the cached structured text directly. `pdf_get_text` similarly uses the cached stext via `fz_copy_selection`.

### `stress-search-cache` Stress Test (Phase 12.2)
A third stress test exercises the cache and asserts ≥1.5× warm/cold speedup on a full-document search. Catches regressions cleanly — without the cache, warm and cold passes would run at the same speed and the test would fail. Registered in `meson test` and runs in <1 s on the Effective Java sample.

The test takes a document path and a query string as args; the registered case uses `"class"` against Effective Java for stable hit counts (1922 hits across all 901 pages).

### What This Doesn't Do (Yet)
The "bonus" item from the roadmap entry — opportunistic stext extraction during render, à la Sumatra's `RenderCache.cpp:790` — stays open. The current design extracts on first text-call rather than during render, so a freshly-opened document's first search is still cold. Adding render-time pre-warm would shift that cost off the search latency path, useful for users who scroll through a doc and then search. Tracked in roadmap Phase 11 Tier 1 as a follow-up.

---

## v0.17.0 (2026-04-30)

*Phase 11 Tier 1 — `fz_cookie` mid-render abort.* Workers now plumb a per-render `fz_cookie` through to `fz_run_page`, and `pdf_cancel_render` flips `cookie->abort = 1` from the main thread. MuPDF sees the flag at its next checkpoint inside `fz_run_page` and abandons the in-flight render with whatever partial state it has. On a 50 MB scanned PDF this saves 1–3 seconds per stale page when the user has already scrubbed away. Neither zathura nor sioyek does this — Framework leads the field on this one.

---

### `fz_cookie` Plumbed Through PDF Render Path
The MuPDF `fz_cookie` is the canonical cancellation primitive: a struct whose `abort` field MuPDF reads periodically during `fz_run_page` execution. Set it from any thread; the next checkpoint inside MuPDF returns immediately. The previous code passed `NULL` for the cookie, so `fz_run_page` always ran to completion regardless of whether the user had scrolled away.

The implementation:
- Each worker allocates an `fz_cookie` on its stack inside `pdf_render_page` and registers the pointer in a new `active_cookies[MAX_RENDER_INSTANCES]` array on `FwDocumentPdf`, indexed by render slot.
- The cookie pointer is published and deregistered under a new `cookies_lock` mutex — *separate* from the per-instance render lock. This is load-bearing: cancel must reach the cookie pointer without blocking on the render lock that the worker holds during `fz_run_page`. Two locks make the cancel signal travel during render, not after.
- After `fz_run_page` returns, `render_page_direct` checks `cookie->abort` and discards the partial surface if set — returning NULL to the worker, which discards the result via the existing stale-discard path.
- New `pdf_cancel_render` walks `active_cookies` under `cookies_lock` and writes `abort = 1` on every published cookie. Wired in `iface_init`. The PDF backend previously had no `cancel_render` implementation, so this also closes a gap: scroll-aborts are now actually honored by the PDF render path.

The cookie pointer's lifetime is exactly the worker's stack frame. Both publish and unpublish happen under `cookies_lock`, and cancel writes under the same lock — so the pointer is never accessed after the worker's frame is gone.

### Validation
Code paths verified clean under ASan: no use-after-stack, no double-free, no leaks across the worst-case scrub pattern. The synthetic stress-scrub test transitions to SCRUBBING before any worker enters `fz_run_page`, so it doesn't drive the cookie path; a Phase X cookie-abort test would need to render briefly first then transition. Real-world validation is `FW_DEBUG=1` plus the GUI: scrolling fast through a big scanned PDF should now produce `[pdf] cancel_render: aborted N in-flight render(s)` traces, with renders bailing in tens of ms instead of completing the full second-or-more rasterization.

The DjVu and CBR backends keep their existing per-document `cancel_flag` mechanisms — they're not as fine-grained as fz_cookie but the streaming-RAR / single-mutex constraints there already cap parallelism, so the value of mid-render abort is much lower.

### Trade-offs Documented
- The pdf backend gains 8 cookie pointers + a mutex on `FwDocumentPdf`. Memory cost: ~80 bytes per open document. Negligible.
- `cookies_lock` is acquired twice per render (publish + unpublish) plus once per cancel. The publish lock is held for nanoseconds (a single pointer write), so contention with cancel is bounded. No measurable per-render overhead.
- Aborted renders return NULL. The worker's existing stale-discard path handles this — no new code there. Side effect: aborted pages count as "still needs render" in the cache, so the next priority update will re-queue them. This matches the SCRUBBING semantics the user expects.

---

## v0.16.0 (2026-04-30)

*Phase 11 Tier 1 — bytes-aware cache cap.* The page-count `CACHE_WINDOW = 30` introduced in v1.3.3 has gone away — replaced by a byte budget that tracks `stride * height` per cached surface. The Phase 12 stress harness flagged this exact case: a 212-page Berserk volume peaks at ~525 MB of surfaces alone (~2.3 MB/page), while a 900-page Effective Java textbook peaks at ~109 MB (~0.12 MB/page). Page-count caps mis-fit by 20×; byte caps don't.

---

### Bytes-Aware Cache Cap (Phase 11 Tier 1)
`FwCache` now tracks `total_cached_bytes` across every cache entry's `surface` and `prev_surface` slots, accounted live at every store/replace/evict path under the existing mutex. Default cap is 512 MB, overridable per-process via the `FW_CACHE_BYTES_CAP_MB` env var. The cap is a soft target on the rendered-surface tier (Tier 2) — parsed handles (Tier 1) and thumbnails (Tier 0) are tracked separately with their own bounds.

**Eviction policy changed.** Previously: pages outside the priority window were unconditionally dropped, so the cache held at most ~21 surfaces. Now: outside-priority surfaces are *kept* until `total_cached_bytes` exceeds the cap, then evicted (oldest hash-iteration first; visible/priority pages are never evicted). The trade-off: scrolling back into a previously-rendered region is now instant (no re-render) when there's headroom, at the cost of higher steady-state memory.

For the case where even the priority window's surfaces exceed the cap (poster-format PDFs at 400% zoom), eviction can't free enough — slicing (Phase 11 Tier 2) is the proper fix and remains scoped there.

`FW_DEBUG=1` now prints `byte-cap evict` lines naming pages dropped, bytes freed, and remaining vs cap. Useful for tuning.

### Cache Constants Cleaned Up
- `CACHE_WINDOW = 30` deleted. Its dual purpose (priority array bound + eviction bound) split: `MAX_PRIORITY_PAGES = 64` is the array bound (only used as a safety check; actual content is `n_visible + 2 * NEAR_RANGE` ≈ 23), and the byte cap replaces the eviction bound.
- `CACHE_BYTES_CAP_DEFAULT = 512 MB` is the new compile-time default.

### Stress Harness Updated for the New Behavior
`stress-scrub` gained a Phase 4 — a slow walk through the document at one priority shift per 200 ms. Without it, the test never accumulated outside-priority surfaces, so the byte-cap eviction path was never exercised. Stress-scrub also pins `FW_CACHE_BYTES_CAP_MB=128` at startup so eviction *will* fire during Phase 4 — under the 512 MB default the test would just hold everything.

Verified across all six backends (PDF, DjVu, EPUB, MOBI, CBZ, CBR) both natively and under ASan. The Berserk CBZ run was the proof point: 7-13 pages dropped per priority shift, ~80 MB freed each time, cache stays at ~125 MB out of the 128 MB cap. No leaks under ASan.

The `stress-scrub` test's RSS cap was bumped from 800 MB to 1200 MB — under the new policy the cache legitimately keeps more surfaces and total RSS includes glibc retention + thumbnails + ASan overhead.

---

## v0.15.0 (2026-04-30)

*Phase 12 — stress harness foundation.* The first piece of the regression net: a `tests/` tree, a `-Dstress=true` meson option that gates the harness, a `-Dsanitize=` option for ASan/UBSan/LSan/TSan builds, and one real stress test that exercises today's Phase 11 Tier 1 cache pipeline. The remaining Phase 12 items (zoom storm, multi-doc, corpus soak, benchmarks, gdb pretty-printers, trace replay, --self-test) stay open as future work.

---

### Engine as a Static Library
`src/meson.build` was refactored to compile the framework's internal modules into a `framework-core` static library; the `framework` executable now links against it via a single `framework_lib_dep`. This makes internal symbols (`fw_cache_*`, `fw_document_*`, `fw_view_*`, etc.) reachable to tests without exposing them as a public API. The `framework` binary itself is unchanged at runtime.

### `-Dstress=true` and the `tests/` Tree
A new top-level `tests/` directory holds the harness. It builds only when `-Dstress=true`, so packagers and end users pay zero cost. `tests/corpus.json` is the canonical sample manifest (default root: `/home/bdkl/docs/Calibre Library`, override via `FW_TEST_CORPUS_ROOT` for portability), tagged so each stress/bench tool can pick the samples it cares about (`large`, `textbook`, `djvu`, `scanned`, `reflow`, `comic`).

```sh
meson setup builddir -Dstress=true
meson compile -C builddir
meson test -C builddir            # once registered targets stabilize
```

### `stress-scrub` (Phase 12.2)
The first stress test. Drives `FwCache` directly without a widget tree and simulates a punishing scroll pattern: 0 → last page in 500 ms, then 5 × back-and-forth, then a 3-second settle. Asserts no crashes (segfault → non-zero exit), peak RSS under a configurable cap (`FW_STRESS_RSS_CAP_MB`, default 800 MB), and no stuck workers.

**Run across all six backends** (PDF, DjVu, EPUB, MOBI, CBZ, CBR) — every backend passed cleanly both natively and under `-Dsanitize=address`. The full corpus-coverage results revealed two real signals: (1) CBZ on a 212-page Berserk volume peaks at 789 MB under ASan, dangerously close to the 800 MB cap, giving the bytes-aware cache cap (Phase 11 Tier 1) concrete weight; (2) the CBR backend's streaming-RAR cost is real — even a single 4-second test renders only a handful of pages on a 583-page comic.

### `stress-zoom-storm` (Phase 12.2)
A second stress test exercising a different code path: the v1.4 `prev_surface` stash and v1.5 texture-before-surface unref ordering. Pins priority on a single page and runs 50 zoom cycles across 25%–400%, alternating direction. Each cycle bumps `render_gen` and triggers the surface/texture replacement. The peak during transition is permitted to be high (~1.2 GB on a 901-page textbook); the **leak signal** is current RSS read from `/proc/self/status` after a 5-second settle — `getrusage`'s high-water mark never decreases and would mask correct lifecycle behavior. Verified clean natively and under ASan; the post-storm RSS drops from ~1218 MB peak to ~660 MB, confirming transient memory is correctly released.

Both tests are registered with `meson test` and run in under 12 seconds combined.

Future stress tests (`stress-multidoc`, `stress-corpus-soak`) and the bench/triage stack remain open in Phase 12 — landing them is later work.

### `-Dsanitize=` Option (Phase 12.4 partial)
A meson `array` option taking any of `address`, `undefined`, `leak`, `thread`. Forwarded to compile and link as `-fsanitize=` flags via `add_project_arguments` and `add_project_link_arguments`. Builds cleanly with `-Dsanitize=address` on Brandon's Fedora; `-Dsanitize=undefined` requires `sudo dnf install libubsan` (not currently installed). Leak and Thread sanitizers similarly need their runtime libs.

`stress-scrub` was rerun under `-Dsanitize=address` against the same 901-page textbook: clean. No use-after-free, no buffer overflows, no leaks. The cache + render-worker pipeline shipped in v0.14.0 holds up under the worst-case scroll pattern with ASan watching.

### Trade-offs Documented
- The static-library refactor adds a minor link-time cost. Functionally invisible at runtime.
- `tests/` is gated behind `-Dstress=true` precisely so this doesn't bloat the standard build. The default `meson setup builddir` produces exactly the same artifacts as before.
- The corpus manifest hardcodes the default path. CI use will require setting `FW_TEST_CORPUS_ROOT` (currently consumed by stress-scrub via argv only — to be wired up properly when the corpus-aware tests land).

---

## v0.14.0 (2026-04-30)

*Phase 11 Tier 1 — render pipeline.* Three pre-1.0 cache-pipeline items land together as a single coherent change: GThreadPool sort-function priority, the symmetric ±10 parsed window, and the per-event scroll cap (toggleable). Together they deliver the user-visible target Brandon framed it as: *normal reading scroll never paints thumbnail placeholders; thumbnails are reserved for explicit jumps.*

---

### GThreadPool Sort-Function Priority Dispatch (Phase 11 Tier 1)
Render jobs now carry a `last_view_time` field and the pool runs `g_thread_pool_set_sort_function (render_job_compare)` — workers naturally pick the most recently prioritized page next, regardless of when its job was pushed. New high-priority pushes (the current viewport on a fresh scroll) jump to the front of the queue ahead of older queued jobs from a previous priority list. Pure GLib pattern borrowed from zathura's `render.c:94`. Replaces the old "walk `priority_order[]` in index order, push one at a time, throttle by `active_jobs < job_limit`" model.

### Startup-Blur Regression Fixed (Phase 11 Tier 1)
The companion regression — saved-state open landing on a thumbnail-blurred page until the user scrolled — turned out *not* to be obviated for free by the sort-function change as predicted. Trace logs showed the priority pool happily rendering pages 0–13 from the initial open and never receiving the saved-page priority update. Root cause: `update_cache_priority` in `fw-view.c` bailed early on `gtk_widget_get_height (self) <= 0`. During the deferred `restore_state_tick`, the adjustment's value-changed signal fires before the view widget's allocation has settled, so the priority update never reached the cache.

The fix is a fallback in `update_cache_priority`: when `widget_height <= 0`, derive the page at the scroll position from `page_y_offsets[]` directly and push that single page as priority. With the sort function in place, those jobs sort ahead of the in-flight pages 0–13 jobs at the next worker handoff, and the saved page renders within a few hundred milliseconds of open — visible by the time the window appears, no scroll required.

### Symmetric ±10-Page Parsed Window (Phase 11 Tier 1)
`fw_cache_set_priority` now builds a symmetric ±10 priority window in all non-SCRUBBING states: visible pages first, then forward/backward interleaved one page at a time outward up to 10 each side. Replaces the previous asymmetric "+7 forward, -3 backward in CRUISING; full 30-page radial outward in STATIC." With sort-function priority dispatch the asymmetric tiering is no longer needed — every push carries a fresh timestamp, so visible-first ordering happens naturally. Total preload window is ~21 pages (visible + 20 neighbors), well under the existing 30-page `CACHE_WINDOW` eviction bound. Parsed handles are lightweight; the slight memory bump is negligible.

The 150 ms CRUISING throttle on priority rebuilds is gone — sort-function handles ordering, no need to throttle priority computation.

### Per-Event Scroll Cap with Kinetic Toggle (Phase 11 Tier 1)
A `GtkEventControllerScroll` on the view (capture phase) now caps single scroll-event deltas at 90 px, applying the bounded delta directly to the vadjustment and consuming the event. Wheel ticks are converted from unit-scale (~1 per click) to pixels via `SCROLL_WHEEL_STEP = 60` first, then clamped. Trackpad smooth-scroll arrives in pixels already. Net effect: per-event flicks can't outrun the render cache during continuous reading scroll.

This trades GTK's kinetic momentum scrolling (the trackpad-flick coast) for predictable cache behavior. Because some users genuinely want the flick — *"if someone is just gliding through research or school things"* — the cap is gated by a new GSettings key:

- **Schema:** `kinetic-scrolling` (bool, default `false`) on the previously-empty `io.github.virinvictus.framework` schema. The skeleton schema now backs one real feature.
- **Menu entry:** "Kinetic Scrolling" in the primary menu, wired to the GSettings key via `g_settings_create_action`. The menu checkmark stays in sync with the setting; toggling flips the behavior live, no restart needed.
- **Behavior with kinetic ON:** the scroll handler returns FALSE, GtkScrolledWindow's default kinetic momentum scrolling takes over. SCRUBBING-state thumbnail behavior still applies if velocity goes high enough.

Default is off (cache-friendly); on is opt-in. Brandon's scope-discipline rule about declaring schema keys ahead of features is honored — this key declaration ships *with* the code that reads it, not before.

### Trade-offs Documented
- **Explicit jumps still flash thumbnails** (TOC click, page-entry edit, internal link click, search-hit reveal). That's the documented trade-off — pre-rendering every chapter target is wasteful, and the user expects a tiny pause on a deliberate jump.
- **Scrubbing state still aborts** with thumbnail placeholders. Kinetic-on users who flick hard enough to trigger scrubbing get the same behavior as before.
- **The `active_jobs` / `max_jobs` counters are now observability-only** — they're no longer read for concurrency limiting. The pool's worker count caps concurrency naturally. Counters retained for debug tracing.

---

## v0.13.0 (2026-04-30)

*Phase 10 — The 1.0 Release.* All shipping artifacts now exist and validate. Framework builds, installs, and runs as a Flatpak end-to-end. The remaining 1.0 work is a tag and a Flathub submission, both of which wait until the broader roadmap is closer to done.

---

### App-ID Rename: `io.github.virinvictus.framework`
The application's reverse-DNS identifier is now `io.github.virinvictus.framework` (was `com.github.vrnvctss.framework`). The reverse-DNS convention requires that the prefix be a domain the developer controls — `com.github.virinvictus` would imply ownership of `virinvictus.github.com` (a subdomain that doesn't exist; GitHub gives users paths, not subdomains under github.com), while `io.github.virinvictus` reverses to `virinvictus.github.io`, the GitHub Pages domain that actually belongs to the developer. Flathub's reviewers explicitly require this form for projects without their own DNS.

The rename touches every artifact keyed on the ID: the desktop file, AppStream metainfo, GSettings schema, scalable icon, Flatpak manifest, the `APP_ID` constant in `meson.build`, the GSettings schema path (`/io/github/virinvictus/framework/`), and the documented references in `spec.md`, `roadmap.md`, and the project `CLAUDE.md`. State persisted under the old ID is invalidated; document state is keyed by file path in `~/.local/share/framework/state.json` and is unaffected.

### AppStream Metainfo Rewrite (Phase 10)
`data/io.github.virinvictus.framework.metainfo.xml.in` rewritten end-to-end. The previous version still described the app as "PDF and DjVu" only and stopped at v1.2.0 in its release history, ignoring the v0.6.0 version reset and everything since. The rewrite includes: current format list (PDF, DjVu, EPUB, MOBI, FB2, XPS, CBZ/CB7/CBT/CBR), feature bullets, `<developer>` block, `<categories>` (Office, Viewer, GNOME, GTK), `<recommends>` (display size ≥ 600 px, offline-only network), `<supports>` (pointing/keyboard/touch), and release entries from v0.6.0 → v0.13.0 in honest versioning. The historical 1.x entries are preserved in this file (`patchnotes.md`) but are not surfaced to software centers — those releases happened, but the running version is honest. `appstreamcli validate` is clean.

A `<screenshots>` block sits in the metainfo as a commented-out template; before any Flathub submission, screenshots will need to be added under `data/screenshots/` and the URLs uncommented.

### Desktop File Polish (Phase 10)
`Comment=` updated to the current format list. New `Keywords=pdf;djvu;epub;mobi;fb2;xps;cbz;cbr;comic;viewer;reader;document;mupdf;` line so software centers and search bars rank Framework correctly. Categories normalized to `GTK;GNOME;Office;Viewer;`. `desktop-file-validate` is clean.

### GSettings Schema Pruned to Skeleton (Phase 10)
The previous schema declared eight keys (`default-zoom-mode`, `default-zoom-level`, `continuous-scroll`, `default-view-mode`, `invert-colors`, `window-width`, `window-height`, `window-maximized`, `sidebar-visible`, `sidebar-width`) that no code in `src/` actually reads. Pre-1.0 is the only safe time to prune a published schema — once 1.0 ships, removing keys becomes a back-compat issue. The schema file now contains only the schema declaration, ready to accept keys when corresponding features are wired up.

### Flatpak Manifest (Phase 10)
`io.github.virinvictus.framework.yml` lives at the project root and builds a working Flatpak end-to-end against `org.gnome.Platform//50` and `org.gnome.Sdk//50`. Three modules: `djvulibre` (autotools, `--disable-static --disable-desktopfiles`), `mupdf` (the project Makefile with `HAVE_X11=no HAVE_GLUT=no HAVE_LIBCRYPTO=no shared=yes USE_SYSTEM_LIBS=no` — bundled third-party libs are simpler than runtime equivalents), and `framework` itself (meson, release buildtype). `libarchive` comes from the freedesktop runtime under GNOME 50, no module needed.

`finish-args` are intentionally tight: no network, no broad filesystem, GPU access via `--device=dri`, Wayland with X11 fallback, and read-only access to `xdg-documents` / `xdg-download` / `xdg-desktop` for command-line invocations. Anything outside those three XDG paths reaches Framework through the Document portal automatically (GtkFileDialog and drag-and-drop both go through it). Permissions audit clean — no `--filesystem=host`, no `--share=network`, no D-Bus talk-names beyond what the SDK auto-includes.

Local install:
```sh
flatpak-builder --user --install --force-clean build-flatpak io.github.virinvictus.framework.yml
flatpak run io.github.virinvictus.framework
```

The Flathub submission step (changing the `framework` module's `type: dir` source to a `type: git` source pointing at a tagged release) deliberately stays open.

---

## v0.12.0 (2026-04-30)

*Phase 9 — Session Resilience.* Closing out the last two pre-1.0 items in the session-resilience phase: a Document Properties dialog and a Keyboard Shortcuts dialog. The 1.0 path now narrows to Phase 10 (Flatpak, AppStream, release).

---

### Document Properties (Phase 9)
A new `Document Properties…` menu entry opens an `AdwDialog` summarizing the active document. Two groups: **Document** (Title, Author, Subject, Keywords, Creator, Producer, Created, Modified — empty rows are auto-hidden, so books with thin metadata get a sparse display instead of "Unknown" placeholders) and **File** (Filename, human-readable Size via `g_format_size`, full Location, Format, Encryption, Pages).

Backed by a new `get_metadata` method on `FwDocumentInterface` returning a `GHashTable<gchar*, gchar*>` of normalized keys. The PDF backend implements it via `fz_lookup_metadata` for `info:Title` / `info:Author` / `info:Subject` / `info:Keywords` / `info:Creator` / `info:Producer` / `info:CreationDate` / `info:ModDate` plus `FZ_META_FORMAT` and `FZ_META_ENCRYPTION` — one implementation covers PDF, XPS, EPUB, FB2, MOBI, and CBZ/CB7/CBT. PDF date strings (`D:YYYYMMDDHHmmSSOHH'mm'`) are parsed to a human-readable `YYYY-MM-DD HH:MM:SS ±HH:MM` form before display. The DjVu and CBR backends return NULL — neither format exposes document-level metadata cleanly, and the File group's filename + size + page count + extension-derived format is sufficient there.

Subtitle text on every row is selectable, so users can copy the title or producer string out of the dialog.

### Keyboard Shortcuts Dialog (Phase 9)
`Ctrl+?` and `F1` now open a Keyboard Shortcuts dialog, also accessible from the primary menu. Built as a custom `AdwDialog` containing an `AdwPreferencesPage` with one group per category (File, Navigation, Zoom & Rotation, Search, View, Selection); each binding is an `AdwActionRow` with a `GtkShortcutLabel` suffix that renders the accelerator with platform-appropriate key glyphs. Wired to the conventional `win.show-help-overlay` action name so a future `GtkShortcutsWindow` swap is a one-handler change.

`GtkShortcutsWindow` itself is deprecated in GTK 4.18; the libadwaita-styled dialog avoids accruing that debt and matches the Document Properties dialog visually.

---

## v0.11.0 (2026-04-30)

*Pre-Phase-9 cleanup.* Four roadmap items that had been left open across earlier phases all land in this release.

---

### XPS + EPUB Format Routing (Phase 7)
The factory now dispatches `.xps` / `.oxps` / `.epub` / `.fb2` / `.mobi` to the MuPDF backend. The MuPDF backend already calls `fz_register_document_handlers` and `fz_open_document` — the work was extending the factory and calling `fz_layout_document(ctx, doc, 600, 900, 11)` for reflowable formats. `fz_is_document_reflowable` decides whether to layout: PDF/CBZ/XPS skip the call (they're fixed-layout); EPUB/FB2/MOBI take the 600×900pt @ 11pt layout. Each render-instance opens its own document so each must call `fz_layout_document` independently — without that, different render threads see different page bounds.

The file-dialog filter and desktop-entry MIME types learned the new extensions and types (`application/oxps`, `application/vnd.ms-xpsdocument`, `application/epub+zip`, `application/x-fictionbook+xml`, `application/x-mobipocket-ebook`).

**EPUB caveat.** Framework's pagination is whatever MuPDF's default layout produces — page breaks happen wherever the layout engine puts them, and the layout doesn't reflow on zoom. Tested cleanly on Wyrd Sisters (159-page output). For serious EPUB reading, [Foliate](https://johnfactotum.github.io/foliate/) handles reflow and font customization properly; Framework is the right choice when you want a single reader for fixed-layout PDFs and EPUBs alongside.

### CBR via libarchive (Phase 7)
Real CBR support without the `libunrar` licensing trap. A new backend (`src/fw-document-cbr.c`, ~370 lines) uses `libarchive` (BSD, GPL-compatible, already on every Fedora install) to enumerate image entries inside the RAR, sorts them by filename (= page order in every comic dump), and on `render_page` extracts that entry's bytes and feeds them to MuPDF via `fz_new_image_from_buffer` → `fz_fill_image` into a draw device wrapping the cairo surface buffer (the same v1.6 zero-copy pattern the PDF backend uses). RAR4, RAR5, ZIP, 7z, and tar archive formats all work through libarchive — the factory currently only routes `.cbr` here, but the backend is format-agnostic.

Threading is the single-mutex pattern, not the PDF backend's 8-instance pattern. libarchive readers can't be safely shared across threads, and the streaming-RAR cost makes per-render archive opens dominate anyway. Each render call opens a fresh `archive *`, walks to the target entry, extracts, and closes. The velocity engine + thumbnail tier hide most of the cost during normal scrolling. Sustained scrub-to-end on a huge archive remains slow — that's a fundamental property of streaming RAR, called out as future work in the file's threading-comment block.

The libunrar-hint error message is gone from the factory; its job is done.

### Embedded File Extraction (Phase 6)
PDF /EmbeddedFiles name-tree extraction. Two new methods on `FwDocumentInterface` — `get_attachments` returns a `GArray<FwAttachment*>` with each attachment's filename, MIME type, and size; `save_attachment` writes one to a destination path. The PDF backend walks the xref via `pdf_xref_len` + `pdf_load_object` + `pdf_is_embedded_file` + `pdf_get_filespec_params` (the zathura-pdf-mupdf reference pattern at `attachment.c`), and saves via `pdf_load_embedded_file_contents` + `fz_save_buffer`. DjVu and CBR backends return NULL — no equivalent attachment mechanism in those formats.

A new menu entry **"Save Embedded Files…"** asks for a destination folder via `GtkFileDialog::select_folder` and saves every attachment under sanitized basenames. The sanitizer strips directory components (defeats `../../../etc/passwd`-style names), drops leading dots (no surprise dotfiles), and replaces control characters — the user-chosen output directory stays the boundary even on attacker-crafted PDFs. A summary `AdwAlertDialog` reports how many files were saved or which failed; PDFs with zero attachments show "No Embedded Files" instead.

### GtkListView Migration of the TOC Sidebar (Phase 8)
`fw-sidebar.c` rewritten end-to-end against `GtkListView` + `GtkTreeListModel` + `GtkSingleSelection`, replacing the deprecated `GtkTreeView` + `GtkTreeStore`. The build's `-Wdeprecated-declarations` warnings on the sidebar are gone. Item type is a new `FwTocItem` GObject (title, page, optional children GListStore) built from the existing `FwTocNode` tree on TOC load. The `GtkTreeListModel`'s create-child-model callback hands back each item's `children` store on demand, so the tree expands lazily.

The v0.8 current-page highlight survives the migration, with one structural change: `find_best_match` now walks the underlying `FwTocItem` tree (not the flat tree-list-model, since rows for collapsed branches don't exist in the flat model). Once it picks the deepest match, a `build_path` walk produces the root-to-target path of `FwTocItem*` pointers; the highlight code expands each ancestor's `GtkTreeListRow` so the target row materializes in the flat model, then walks the model to find the row's position and calls `gtk_single_selection_set_selected` + `gtk_list_view_scroll_to`. Click navigation routes through `GtkListView::activate` → `page-requested` signal — same contract the window has handled since v1.0.

---

## v0.10.0 (2026-04-30)

---

### Empty Window State (Phase 8)
Launching Framework with no file argument now shows an `AdwStatusPage` with the app icon, the prompt "Open a Document", and a suggested-action pill button wired directly to `app.open`. The page also tells the user they can drop a file onto the window — pulling double duty as documentation for the new drag-and-drop handler. The split view's content is now a `GtkStack` that crossfades between the empty page and the document overlay; on `fw_window_open_file` success it switches to the document view, and stays there for the window's lifetime (no reverting to empty when a doc closes — opening a new file replaces the current document, matching the rest of the single-document-per-window model).

### Drag-and-Drop File Open (Phase 8)
A `GtkDropTarget` accepting `G_TYPE_FILE` is attached to the window. On drop, the file's path is resolved with `g_file_get_path` and routed through the standard `fw_window_open_file` path — so a dropped file replaces the current document if one is open, or boots the document view from the empty state if not. Multi-file drop (drag a folder of comics onto the window) is intentionally out of scope here because Framework is one-document-per-window; multi-window file open already exists via `g_application_open` from the command line.

### Printing (Phase 8)
`Ctrl+P` now opens the system print dialog. Printing routes through `GtkPrintOperation`: `begin-print` reports the page count from the active document, `draw-page` renders the requested page via the existing `fw_document_render_page` interface (so PDF, DjVu, and CBZ all print through the same code path), and the resulting cairo surface is painted into the print context with a `cairo_scale (cr, 1/zoom, 1/zoom)` so 1 source pixel maps to 1 point on paper. Render quality is the print context's reported DPI capped at 300 — a US-letter page renders to ~33 MB, plenty for laser/inkjet output without ballooning memory on a long print job. The print dialog uses the document's basename as the job name so spool queues stay readable.

### Out of Scope This Release
**`GtkListView` migration of the TOC sidebar** stays open. The `GtkTreeView` API is technically deprecated in GTK4, and the build prints one `-Wdeprecated-declarations` per compile, but it works correctly and our v0.8 TOC-highlight walker depends on the `GtkTreeIter` traversal API. Migrating means rewriting both `populate_store` and `find_best_match` against `GtkTreeListModel` — pure churn, zero new user value, with regression risk on a feature we just shipped. Deferred to a future release where we have a reason to be in that file.

---

## v0.9.0 (2026-04-30)

---

### Comic Book Archive Support — CBZ (Phase 7)
Framework now opens CBZ comic-book archives. The PDF backend was already format-agnostic — it calls `fz_register_document_handlers` and `fz_open_document`, both of which dispatch by format internally — so the work was almost entirely factory-side: extend `fw_document_new_for_path` to accept `.cbz` / `.cbr` / `.cb7` / `.cbt`, route them through the same MuPDF backend, and tag the trace label as "Comic (MuPDF)" so logs make sense. The 8-instance parallel render path, velocity engine, thumbnail tier, and search infrastructure all carry over for free — opening a 237-page Berserk volume gets the full multi-core render treatment with zero new code in the cache layer.

A 237-page CBZ volume verified: opens in ~14 ms, the first ten pages immediately enter the parsed-handle window, and scrolling stays smooth under the existing velocity engine. Search returns no results (CBZ pages are images, no text layer), Match Count correctly reports zero, and the rest of the UI is identical to PDF behaviour.

### CBR — Best-Effort with Actionable Error Message
`.cbr` is also accepted by the factory, but the file format is RAR-compressed and most Linux distributions (including Fedora) ship MuPDF without the optional `libunrar` dependency for licensing reasons. Trying to open a CBR now produces a tailored error dialog explaining the situation and suggesting CBZ conversion, instead of MuPDF's bare "cannot find document handler". The CBR path is one `g_set_error_literal` swap in the factory — when a libunrar-enabled MuPDF is available, CBR will Just Work with no further code changes.

### File Dialog & Desktop Entry MIME Wiring
The Open dialog filter learned `*.cbz` / `*.cbr` / `*.cb7` / `*.cbt` and the corresponding MIME types (`application/vnd.comicbook+zip`, `application/vnd.comicbook-rar`, `application/x-cbz`, `application/x-cbr`). The `.desktop` file's `MimeType=` line gained the same set so file managers offer Framework as a handler for comic archives.

### Out of Scope This Release
EPUB / XPS / FB2 / MOBI — MuPDF supports them and the factory could trivially route them through the same backend, but each has format-specific UX considerations (EPUB reflow, XPS per-page sizing, MOBI proprietary parsing edge cases) that deserve their own review. Phase 7 specifically scopes "graphic novels" first; the rest stays open.

---

## v0.8.0 (2026-04-30)

---

### TOC Highlight Tracking (Phase 6)
The sidebar now follows along as the user scrolls. `fw_sidebar_set_current_page` walks the TOC tree depth-first looking for the deepest entry whose destination page is ≤ the current page, then selects that row and scrolls it into view. The walk is recursive but bounded by the document's TOC depth (chapter / section / subsection — three deep on every textbook tested), so calling it from every scroll-tick `value-changed` callback is cheap. Ancestor nodes are auto-expanded so a deeply-nested section actually becomes visible. Programmatic selection in GTK4's `GtkTreeView` does not trigger `row-activated`, so there is no feedback loop with our existing TOC click handler.

### Navigation History (Alt+Left / Alt+Right)
Standard browser-style back/forward stacks for in-document jumps. The window keeps two `GArray`s of `(page, scroll-fraction)` entries. The back stack is pushed when the user makes an *explicit* jump — a TOC click, a page-entry edit, or an internal-link click — and the forward stack is wiped at the same moment, matching every web browser's history rule. Plain scrolling, the next/previous-page buttons, and search-hit reveal do *not* push, because scrolling away from your current spot to read more is not a "jump." Alt+Left pops back, pushing the current viewport onto forward; Alt+Right pops forward, pushing onto back. The history is per-window and cleared on document switch.

### `page-jumped` Signal on `FwView`
The view emits `page-jumped(int dest_page)` only when an internal link click triggers a navigation. The window subscribes to push the previous viewport onto the back stack — without this, link-click jumps were invisible to the navigation code (the click bypassed `go_to_page`). Search-hit reveals deliberately stay silent so navigating through 47 search matches doesn't bury the user's pre-search position under a 47-entry stack.

### Sidebar Click Navigation (already shipped, now formally complete)
TOC click navigation has worked since v1.0 via `GtkTreeView::row-activated` → `FwSidebar::page-requested` → `FwWindow::on_sidebar_page_requested`. Phase 6 marks it formally complete; the TOC click path now also pushes navigation history.

---

## v0.7.0 (2026-04-30)

---

### Async, Progressive Search (Phase 5)
Search no longer blocks the UI. The previous `fw_search_find` was a synchronous loop calling `fz_search_page` on every page in turn — on a 1000-page textbook this froze the window for several seconds before any result appeared. The new path runs the page-by-page scan on a dedicated worker thread and posts each page's hits back to the main loop via `g_idle_add_full`, so matches appear as they're found and the UI stays responsive throughout. The scan also starts at the user's current page and wraps, so matches near where they're reading appear first.

A monotonically-incrementing generation counter discards in-flight messages from cancelled scans — typing into the search bar instantly retargets the worker without races. Cancellation is cooperative via an atomic flag the worker polls between pages; cleanup is deterministic on document close, dispose, or query change.

### Search Result Highlighting
All hits paint as semi-transparent yellow overlays directly in `fw_view_snapshot`, scaled and translated into widget coordinates via the same zoom/page-position math the cache uses. The active hit (the one the count label says is "current") paints in a stronger orange tint instead of yellow so the user always knows which match `Next`/`Prev` will move them away from. Highlights re-layer correctly under text selection and link cursors, and are invalidated automatically by the existing `redraw_pending` flag — no extra repaint plumbing.

### Search Navigation (F3 / Shift+F3)
F3 jumps to the next match, Shift+F3 to the previous, with wrap-around at both ends. Both are exposed as `win.find-next` / `win.find-prev` GActions so they work whether or not the search bar is focused, and the search entry's built-in next-match/previous-match signals route to the same handlers. When the active hit changes, the view scrolls so the hit lands roughly one-third of the way down the viewport (reading context above it) and pans horizontally if the hit is offscreen due to zoom.

### Match Count Label
The search bar now shows "3 of 47" alongside the entry. While the worker is still scanning, the count appends a `+` ("3 of 47+") and the label switches to "Searching…" while results are still empty, so the user can tell the difference between "no matches yet" and "no matches at all." The Prev/Next buttons disable when no hits exist.

### `FwSearch` API Reshape
The signal-based interface is new: `hits-changed`, `current-changed`, `search-finished`. `fw_search_find()` now takes a `start_page` argument; `fw_search_clear()` is split out from `set_document` and is also called when the search bar closes; new helpers `fw_search_hits_for_page`, `fw_search_peek_hits`, `fw_search_active_index`, and `fw_search_get_current_page` let the view read state without re-iterating. `FwView` gained `fw_view_set_search` / `fw_view_reveal_active_hit` and a new owned ref to the search controller.

### Roadmap: Phase 12 (Stress-Testing & Debugging Suite)
Added a new top-level roadmap phase covering a `tests/` tree with stress tests (`stress-scrub`, `stress-zoom-storm`, `stress-multidoc`, `stress-corpus-soak`), benchmarks (`bench-render`, `bench-cache-hit-rate`, `bench-startup`), and a debugging setup (`-Dsanitize` meson option, `tests/scripts/debug.sh`, `coredump-triage.sh`, an `FW_DEBUG` log-replay tool, and `framework --self-test`). All of it is gated by a `-Dstress=true` meson option so packagers don't pay for it. None of it is built yet — the phase exists so the Phase 11 borrows have a regression net to land into.

---

## v0.6.0 (2026-04-29)

### Pre-1.0 Version Regression
Dropped the project version from `1.6.0` to `0.6.0`. The earlier 1.x numbering implied a stability and feature-completeness the project hasn't earned: Framework opens and reads PDF and DjVu correctly, but search is synchronous and incomplete (Phase 5), TOC navigation is partial (Phase 6), printing isn't wired up (Phase 8), no Flatpak ships (Phase 10), and the reference-survey borrows in Phase 11 (`fz_cookie` cancellation, cached stext, bytes-aware cache, hue-preserving recolor) are still TODO. A 1.0 tag should be earned at the end of Phase 10, not assumed at the start. Past patchnotes entries keep their historical 1.x labels — those releases happened — but the running version is now honest.

---

## v1.6.0 (2026-04-17)

---

### Zero-Copy MuPDF Render
Replaced the MuPDF → cairo conversion pipeline entirely. The previous path rendered into an intermediate `fz_pixmap`, then walked every pixel in a scalar loop to shuffle RGB → BGRA and premultiply alpha. The new path — borrowed straight from `zathura-pdf-mupdf` — constructs the pixmap *around the cairo surface buffer* via `fz_new_pixmap_with_bbox_and_data` using `fz_device_bgr` as the colorspace. MuPDF's draw device writes rendered pixels directly into the final ARGB32 buffer in the correct byte order, with no intermediate allocation, no channel shuffle, and no per-pixel loop. On a typical 1600×2100 page render this cuts ~15-30% off the per-page wall time and eliminates all per-page `cairo_image_surface_create` + scalar-loop overhead. The `pixmap_to_cairo_surface` helper and its 4-pixel unrolled hot path from v1.5 are now deleted — the optimization is obsolete because we no longer copy pixels at all.

### Unified PDF Render Path
Collapsed the duplicated "parallel instance" and "fallback to main context" code paths in `pdf_render_page` into a single code path that picks the context+document+lock at the top. The two branches now share identical render logic via the new `render_page_direct()` helper. Easier to reason about and less drift risk when future optimizations land.

### Reference Source Study
Downloaded `mupdf`, `djvulibre`, `zathura-pdf-mupdf`, and `zathura-djvu` sources for side-by-side comparison. The zero-copy render path above came directly from studying zathura's implementation. Our DjVu backend was already doing the right thing (RGBMASK32 format matching cairo ARGB32, writing straight into the surface buffer) since v1.0 — zathura's DjVu plugin confirmed the approach is optimal.

---

## v1.5.0 (2026-04-17)

---

### Persistent Thumbnail Tier
Introduced a third cache tier for low-resolution page previews (~150px wide). Thumbnails render in a dedicated single-thread background pool, so they never compete with full-resolution renders for CPU slots. Once rendered they are never evicted — each thumbnail costs ~120KB, so a 1000-page document fits in ~120MB. When a visible page has no full-resolution surface ready (fast scroll, cold cache, mid-zoom-transition), the view now paints the scaled thumbnail instead of a gray rectangle. Users see actual content during fast scroll instead of placeholders.

### Per-Frame Texture Caching
`GdkTexture` objects are now cached inside each `CacheEntry` and reused across frames. Previously, every snapshot pass allocated a fresh `GdkMemoryTexture` + `GBytes` wrapper for every visible page — at 60fps with 3 visible pages, that was ~180 allocations per second. The new path builds the texture once when the render worker stores a surface, holds it for the entire surface lifetime, and drops it atomically when the entry is evicted. The `prev_surface` zoom-transition path has a matching `prev_texture` slot so even scaled placeholders avoid re-allocation.

### Hot-Path Pixmap Conversion
Rewrote `pixmap_to_cairo_surface()` in the PDF backend to hoist branches out of the per-pixel inner loop. The format check (RGB vs. RGBA) now happens once at the top of the function, and the RGB path (the common case for opaque PDFs) is 4-pixel unrolled. For a typical 1600x2100 page render, this cuts ~10-20% off the pixel format conversion time. The compiler can now vectorize the unrolled loop on targets that support it.

### Scroll Velocity Capping
Added a hard cap of 120px on single-event scroll distance (previously unbounded). Combined with the existing SCROLL_STEP damping, this prevents a single fast wheel flick or amplified trackpad event from blowing past multiple pages in one frame. The render cache can now reliably keep up with sustained scrolling without entering the scrubbing-abort state unnecessarily.

### Texture Memory Layout Fix
The previous texture path relied on GBytes's `GDestroyNotify` to eventually free the underlying cairo surface, but the surface destroy order in `cache_entry_free()` was ambiguous. The new code unrefs the texture before destroying the surface — the texture's internal GBytes drops the surface's first reference, then our explicit surface destroy drops the last. This guarantees the GPU-uploaded pixel buffer remains valid for GTK's full rendering lifecycle.

---

## v1.4.0 (2026-04-16)

---

### GPU Color Inversion
Replaced the per-frame `g_memdup2` pixel inversion loop with `gtk_snapshot_push_color_matrix()`. Color inversion now applies a 4x4 matrix on the GPU — zero memory allocation, zero pixel copying. At 60fps with 5 visible pages, this eliminates ~30-60 MB/s of wasted allocations that the old path produced. Both the normal and inverted rendering paths are now fully zero-copy.

### Velocity EMA Smoothing
The scroll velocity tracker now uses an exponential moving average (`0.7 * old + 0.3 * new`) instead of raw per-frame `dy/dt`. Single-frame spikes from mouse wheel clicks or trackpad jitter no longer trigger the scrubbing abort state. Genuine fast scrolling still activates scrubbing correctly — the EMA responds within 2-3 frames.

### Scroll Position Preservation
Zooming in or out no longer jumps to a random position. Before each zoom change, the view records the current page and fractional offset within that page. After the layout recomputes at the new zoom level, the scroll position is restored to the same page and fraction. Sub-page precision is maintained across arbitrary zoom changes.

### Fit-Page Zoom (Ctrl+2)
Implemented `fw_view_fit_page_zoom()` which calculates `min(viewport_w / max_page_w, viewport_h / max_page_h)` across all pages. The entire page fits within the viewport without scrolling. Accounts for rotation — at 90/270 degrees, width and height are swapped before the calculation.

### Rotation (Ctrl+Shift+Plus/Minus)
Document rotation in 90-degree increments. `Ctrl+Shift+Plus` rotates clockwise, `Ctrl+Shift+Minus` rotates counter-clockwise. The view layout swaps page width and height at 90/270 degrees. The cache re-renders all visible pages at the new rotation. Both MuPDF and DjVuLibre backends already supported rotation in their render paths — this release wires it through the UI layer with proper layout recomputation. Rotation state is saved and restored per-document.

### Stale Surface Placeholder
Zoom transitions no longer flash gray placeholders. When the render generation changes (zoom, rotation, or scale factor), existing surfaces are moved to a `prev_surface` slot in the cache entry. The view renders these scaled-to-fit as placeholders until the sharp re-render arrives. The result is slightly blurry content during the transition instead of a blank gray rectangle. Previous-generation surfaces are freed as soon as the new render completes.

### Scroll Damping
Scroll wheel events are now intercepted and applied with a controlled step size (60px per tick), bypassing GTK's kinetic scrolling amplification. This bounds the maximum achievable scroll velocity, reducing the frequency of scrubbing abort triggers and giving the render pipeline more time to keep up. The velocity engine still tracks actual scroll speed via the frame clock tick callback.

### Redundant Redraw Guard
Added a `redraw_pending` flag to the view widget. When the scroll adjustment fires `value-changed` rapidly (every scroll tick), redundant `gtk_widget_queue_draw()` calls are suppressed. The flag is cleared at the start of each `snapshot` call. The render worker's idle-based redraw scheduling is unaffected — it already self-rate-limits via the GLib idle mechanism.

### Text Selection and Copy (Ctrl+C)
Click-drag on a page selects text. A `GtkGestureDrag` on the view widget maps mouse coordinates to document-space points via the page layout's centering and zoom transforms. The selection is rendered as a semi-transparent blue overlay (`rgba(0.2, 0.4, 0.8, 0.3)`) painted after the page texture in the snapshot. On drag end, `fw_document_get_text()` extracts the text within the selection rectangle. `Ctrl+C` copies the selected text to the system clipboard via `gdk_clipboard_set_text()`. Selection is single-page only in this release.

### Dynamic Cursors
The mouse cursor changes based on what it's hovering over. A `GtkEventControllerMotion` on the view maps the pointer position to document coordinates and hit-tests against link rectangles. The cursor shows a pointing hand over links and a text I-beam over page content. Link rectangles are cached per-page and invalidated on document change.

### Link Click Navigation
Clicking a link navigates. Internal links (to other pages in the document) call `fw_view_go_to_page()`. External links (URLs) launch the default browser via `GtkUriLauncher`. The click gesture is registered before the drag gesture — when a link is hit, the click claims the event sequence so text selection doesn't start. When no link is hit, the event falls through to the drag gesture for text selection.

### Debug Tracing Expansion
Added structured trace coverage for all new code paths: scroll position preservation (`view` domain), text selection drag lifecycle (`view`), link click events (`view`), cache I/O page opens (`cache`), and `prev_surface` stash/free events (`mem`). All new traces follow the existing zero-overhead pattern — a single `G_UNLIKELY` atomic check when `FW_DEBUG` is not set.

## v1.3.3 (2026-04-12)

---

### Cache Memory Leak Fix
Fixed `fw_cache_dispose` never running. `FwView` held a GObject ref to the cache via `g_set_object`, but the view's dispose ran too late (or never) in GTK4's widget teardown order — the cache refcount never hit zero. Fixed by explicitly disconnecting the view from the document and cache at the start of `fw_window_dispose`, before dropping the window's own refs.

### DjVu Initial Render Fix
Fixed DjVu pages appearing blank on file open until the user scrolled. `fw_window_open_file` called `set_zoom()` (which internally calls `fw_cache_start()`, bumping the generation counter) and then called `fw_cache_start()` again explicitly — the second call bumped the generation a second time, making the first batch of render jobs stale. For DjVu (serialized single-mutex renders), all queued pages were discarded before any completed. Removed the redundant `fw_cache_start()` calls.

### Split Generation Counter (I/O Optimization)
Split the single `generation` counter into `render_gen` (zoom/rotation/scale changes) and `cancel_gen` (scrubbing/stop abort). Previously, entering scrubbing state bumped the shared generation, invalidating all already-rendered surfaces even though zoom and rotation hadn't changed. With the split, scrubbing only bumps `cancel_gen` to abort in-flight work — completed surfaces rendered at the correct zoom/rotation are kept. Eliminated ~816 wastefully discarded surfaces per heavy scroll session.

### Debug Tracing System
Added zero-overhead runtime debug tracing, enabled with `FW_DEBUG=1`. Domain-prefixed structured logging covers document lifecycle (`doc`), PDF backend (`pdf`), DjVu backend (`djvu`), cache operations (`cache`), view state (`view`), window actions (`window`), and memory events (`mem`). All trace calls compile to a single `G_UNLIKELY` atomic check when disabled. Output goes to stderr with timestamps.

### Cache Window Reduction
Reduced the parsed page cache window from 50 to 30 pages to lower speculative rendering overhead without impacting scroll-ahead coverage.

## v1.3.2 (2026-04-12)

---

### Velocity-Aware Render Throttling
The cache engine now adapts its workload based on scroll velocity. During cruising (moderate scrolling), priority rebuilds are throttled to once per 150 ms instead of every scroll tick, the thread pool is limited to 2 concurrent render jobs (down from all cores), and only the immediate neighborhood (visible + 7 forward + 3 backward) is queued — the full 50-page window waits until scrolling stops. The scrubbing threshold is also lowered from 2000 px/s to 1500 px/s so full render abort kicks in sooner. Result: significantly less CPU churn during fast scrolling through both PDF and DjVu documents.

### DjVu Zero-Copy Rendering
DjVu page rendering no longer allocates a temporary RGB buffer or runs a pixel-by-pixel format conversion. The render format is switched from `DDJVU_FORMAT_RGB24` (3 bytes/pixel into a scratch buffer, then shuffled into ARGB32) to `DDJVU_FORMAT_RGBMASK32` with channel masks matching cairo's native ARGB32 layout. DjVuLibre now writes 32-bit pixels directly into the cairo surface buffer. A fast `|= 0xFF000000` alpha pass and any rotation run outside the render lock, reducing mutex contention. The `ddjvu_format_t` object is created once at document open instead of per-page.

### DjVu Cancel Flag Fix
Fixed DjVu files going permanently blank after fast scrolling. The `cancel_flag` set by the velocity engine's scrubbing state was never cleared in the `render_page_from_handle` code path — once set, every subsequent DjVu render returned NULL. The flag is now cleared on both entry paths.

### Safe Widget Redraw Scheduling
The render worker's `g_idle_add` callback now holds a proper `g_object_ref` on the view widget and checks `GTK_IS_WIDGET` before calling `gtk_widget_queue_draw`. Previously, a raw pointer was passed via `g_idle_add_once`, which could fire after the widget was disposed during document swap — producing infinite `GTK_IS_WIDGET` assertion spam.

## v1.3.1 (2026-04-11)

---

### Cache Freeze After Fast Scrolling
Fixed a bug where the cache would freeze after fast scrolling, requiring a manual zoom to recover. When the velocity engine entered scrubbing state (bumping the generation counter), render jobs that were mid-flight would complete and discard their stale surfaces but leave `rendering = TRUE` on the cache entry. Those pages were permanently stuck — `submit_next_jobs` would skip them, so they never re-rendered. Both the early bail-out (job starts after generation bump) and late bail-out (job finishes after generation bump) paths now clear the rendering flag.

### Smarter Cache Priority
The render priority window now populates the immediate neighborhood first: visible pages, then 7 pages forward, then 3 pages backward, then the rest of the 50-page window. Previously, all forward pages were queued before any backward pages, so scrolling backward hit blank pages even though the cache window was large.

### DjVu Widget Assertion Fix
Fixed an infinite `gtk_widget_queue_draw: assertion 'GTK_IS_WIDGET (widget)' failed` spam when opening DjVu files. The render worker's `g_idle_add_once` was calling `gtk_widget_queue_draw` on the view widget pointer after the widget had been disposed during document swap, or before it was fully realized.

## v1.3.0 (2026-04-11)

---

### Two-Tier Cache Architecture
Replaced the single surface cache with a two-tier system that separates parsed page objects from rendered pixel surfaces.
- **Tier 1 (Parsed Window):** Pre-loads lightweight backend page objects (`fz_page` / `ddjvu_page`) for the entire priority window (~50 pages). Negligible RAM cost, eliminates disk I/O when scrolling into uncached regions.
- **Tier 2 (Pixel Window):** Rendered `cairo_surface_t` surfaces, same as before but now fed by pre-loaded handles.
- **Page Handle API:** New `fw_document_open_page()` / `close_page()` / `render_page_from_handle()` on the document interface. Backends implement the separation between page loading (I/O-bound) and rendering (CPU-bound).

### MuPDF Parallel Rendering
MuPDF rendering is no longer serialized through a single mutex.
- **Independent Instances:** Up to 8 separate `fz_context` + `fz_document` pairs are created at document open, each opening the file independently. Each render thread acquires its own instance via round-robin.
- **Zero Shared State:** Unlike cloned contexts (which share the font/image store), independent instances have no shared state at all. This prevents crashes with PDFs that use JPEG2000 images or complex color spaces where lazy stream reads would race.
- **Result:** On multi-core machines, multiple pages render simultaneously with full thread safety.

### DjVu Render Cancellation
DjVu rendering now supports cooperative cancellation during high-velocity scrubbing.
- **Cancel Flag:** When the velocity engine enters scrubbing state, `cancel_render()` sets a flag on the DjVu backend. The render function checks this flag before and after the expensive page decode step, bailing out immediately if set.
- **Result:** Rapid scrolling through DjVu documents no longer locks the render mutex for the duration of abandoned page decodes.

### Fast DjVu Page Probing
DjVu page dimensions are now pre-cached at document open time, matching the PDF backend's behavior. Previously, every call to `get_page_size()` hit `ddjvu_document_get_pageinfo()`. Now a single loop at open time populates cached arrays, eliminating repeated I/O during layout computation.

### Wayland Fractional Scaling
Render resolution now accounts for the display's device pixel ratio.
- **Scale Factor Awareness:** The cache multiplies the logical zoom by `gtk_widget_get_scale_factor()` when submitting render jobs. Text and graphics are rendered at native display resolution.
- **Monitor Changes:** Moving a window between displays with different scale factors triggers automatic re-render at the correct resolution.

### Invert Colors
`Ctrl+I` now works. Color inversion is applied at the display stage — RGB channels are bitwise-inverted on the pixel data during snapshot, without re-rendering the underlying document surfaces. Toggling is instant with no cache invalidation.

### Ctrl+Scroll Wheel Zoom
Zooming with `Ctrl+Scroll` now anchors to the pointer position rather than the viewport center. The zoom target is calculated from the pointer coordinates relative to the document, so the content under the cursor stays fixed as the zoom level changes.

### MuPDF Thread Safety
Fixed critical crashes (SIGSEGV) when rendering PDFs. The original cloned-context approach shared MuPDF's font/image store across threads — even with proper store locking, `fz_page` and `fz_image` objects lazily read from PDF streams owned by the parent document, corrupting state under concurrent access. Replaced with fully independent render instances that open the file separately per thread, eliminating all shared state.

### Ref-Counted View Pointers
`FwView` now holds proper GObject references to the document and cache objects via `g_set_object()`, preventing dangling pointer crashes on document swap.

## v1.2.0 (2026-04-11)

---

### The Velocity Engine
Replaced the brute-force static cache with intelligent resource pacing. 
- **Velocity Tracking:** The app now actively tracks scroll speed (`dy/dt`) using frame clock ticks.
- **Dynamic Queue Management:** Three render states (Static, Cruising, Scrubbing) automatically adjust the cache window. During high-velocity scrubbing, queued background render jobs are instantly aborted to prevent CPU thrashing.
- **Thread Drip-Feeding:** The worker pool now evaluates velocity after every single page render, preventing queue flooding and memory spikes.

### Memory & Performance Fixes
- **64MB MuPDF Clamp:** Hardcoded the `fz_new_context` store limit from the default 256MB down to 64MB, drastically reducing the baseline memory footprint.
- **Surgical Mutexing:** Cairo surface copying has been moved outside the MuPDF global lock, ending memory doubling during page transit and thread starvation.
- **Safe Exception Variables:** Addressed a critical crash risk by making variables modified in MuPDF's `fz_try` blocks `volatile` to comply with `setjmp/longjmp` rules.

## v1.1.0 (2026-04-07)

---

### State Persistence

Framework now saves and restores per-document state across sessions. On close,
the current page, scroll position, zoom level, and rotation are written to
`~/.local/share/framework/state.json`. Reopening the same file restores
exactly where you left off. Entries older than 90 days are pruned on startup,
capped at 500 documents (LRU).

**Save trigger.** State is saved via the `close-request` signal, which fires
while the window and all its widgets are still alive — not in `dispose` where
adjustments may already be destroyed.

**Scroll restore.** Deferred via `gtk_widget_add_tick_callback` until the
scrolled window has a real allocation and the layout is computed. The saved
page is navigated to first, then the scroll fraction is applied for sub-page
precision.

### Live Page Tracking

The page number in the header bar now updates as you scroll through the
document. Previously it only changed on explicit navigation (Page Up/Down,
go-to-page). A `value-changed` handler on the vadjustment calls
`fw_view_get_current_page` — a reverse lookup through the page y-offset
array — and updates the entry on every scroll position change. Works for
both PDF and DjVu.

### Bug Fixes

**JSON state crash.** `json_node_new(JSON_NODE_OBJECT)` creates a node typed
as object but leaves the internal object pointer NULL. All three fallback
paths in `load_root` now use `json_node_init_object` with a properly
allocated `json_object_new()`.

---

## v1.0.2 (2026-04-06)

---

### Keyboard Shortcuts

All keyboard shortcuts now work. They were previously registered in
`fw_window_constructed` where `gtk_window_get_application()` returns NULL —
the entire shortcut block was silently skipped. Moved to
`fw_application_startup` where the application object is guaranteed to exist.

Full shortcut list: Ctrl+/- (zoom), Ctrl+0/1/2 (actual/fit-width/fit-page),
Page Up/Down (navigation), Home/End (first/last page), Ctrl+G (go to page),
F9 (sidebar), F11 (fullscreen), Ctrl+F (find), Ctrl+I (invert colors),
Ctrl+P (print), Ctrl+O (open), Ctrl+Q/W (quit).

### Arrow Key Scrolling

Arrow keys now scroll the document view. Previously, Up arrow selected all
text in the page number entry because GTK focused the first focusable widget
in the header bar. Fixed with a `GtkEventControllerKey` in `GTK_PHASE_CAPTURE`
on the window — intercepts Up/Down/Left/Right before any child widget sees
them. The view widget is made focusable and given initial focus.

### Grey Flash on Zoom (DjVu)

Zooming DjVu files no longer flashes grey. The cache eviction in
`fw_cache_set_priority` was removing old-generation surfaces before new
renders completed. Eviction now only removes pages whose surfaces have already
been replaced at the current generation — stale surfaces stay visible until
their replacements arrive.

### Menu Items

**About Framework** — shows an `AdwAboutDialog` with app name, version,
description, license (GPL-3.0), website, and issue tracker link.

**Invert Colors** and **Print** — no longer greyed out. Actions are registered
(stubs for now). Previously the menu referenced `win.invert-colors`,
`win.print`, and `win.show-help-overlay` but no matching actions existed.
Removed the Keyboard Shortcuts menu entry (requires a `GtkShortcutsWindow`
not yet built).

### File Dialog

The Open dialog (Ctrl+O) now actually opens the selected file. Previously
`gtk_file_dialog_open` was called with a NULL callback — the async result
was discarded. Wired up `open_file_cb` to receive the result and call
`fw_window_open_file`.

### Version Flag

`framework --version` and `framework -v` print the version and exit.

---

## v1.0.0 (2026-04-06)

---

### Initial Release

First working build of Framework — a fast, native GNOME document viewer built
on MuPDF and DjVuLibre.

**Rendering backends.** PDF rendering via MuPDF with mutex-serialized
`fz_context` access for thread safety. DjVu rendering via DjVuLibre's
`ddjvuapi` with async decode waiting. Both backends implement a shared
`FwDocument` GObject interface with vtable dispatch — backend selection is
automatic based on file extension.

**Pre-cache engine.** A `GThreadPool` renders pages asynchronously into
`cairo_surface_t` surfaces stored in a hash table. Priority ordering: visible
pages first, then pages ahead of the scroll position, then pages behind.
Generation-based invalidation lets zoom/rotation changes cancel stale render
jobs without blocking. Memory is bounded to a 50-page sliding window — distant
pages are evicted to keep RAM in check.

**View widget.** Custom `FwView` implements `GtkScrollable` for proper
integration with `GtkScrolledWindow`. Continuous vertical scroll with kinetic
scrolling. Pages are laid out with cached y-offsets for O(1) page lookup.
Snapshot only paints pages visible in the current viewport. Horizontal scroll
offset applied when content exceeds viewport width.

**Fit-width zoom.** Default zoom on open scales the widest page to fill the
viewport. Calculation is deferred via `gtk_widget_add_tick_callback` until the
scrolled window has a real pixel allocation — no hardcoded fallback dimensions.

**Window.** `AdwApplicationWindow` with full header bar: sidebar toggle, zoom
controls (buttons + editable entry), filename title with tooltip, page
navigation (buttons + editable entry), search toggle, and primary menu.

**Keyboard shortcuts.** Ctrl+/- for zoom, Ctrl+0/1/2 for actual/fit-width/fit-page,
Page Up/Down for navigation, Home/End for first/last page, Ctrl+G for go-to-page,
F9 for sidebar, F11 for fullscreen, Ctrl+F for search.

**TOC sidebar.** `AdwOverlaySplitView` with tree view populated from the
document's outline structure. Click to navigate. Overlays on narrow windows,
sits beside content on wide windows.

**Fast page size reading.** PDF backend uses `pdf_page_obj_transform` to cache
all page dimensions at open time without loading full page objects — eliminates
the multi-second `fz_load_page` loop on large documents.

**DjVu support.** Full backend with page rendering, TOC extraction from
miniexp s-expressions, text extraction, and search. Render access serialized
via mutex.

**Data files.** Desktop entry, AppStream metainfo, GSettings schema (with enums
for zoom mode and view mode), and scalable app icon.
