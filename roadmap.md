# Framework — Roadmap

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

What's done, what's next, what's deferred. Sequenced for maximum performance and a strictly defined 1.0 Wayland/Linux release.

---

## Phase 0 (The "Framework" lololol)
- [x] MuPDF PDF backend with mutex-serialized thread safety
- [x] DjVuLibre backend with async decode, TOC, text, search, links
- [x] Pre-cache engine with thread pool, priority ordering, generation invalidation
- [x] GtkScrollable view with continuous vertical scroll
- [x] Horizontal scroll when zoomed past fit-width
- [x] Fit-width default zoom (deferred until widget allocation)
- [x] Kinetic scrolling
- [x] Arrow key scrolling (Up/Down/Left/Right)
- [x] Page Up/Down, Home/End navigation
- [x] Go to page (Ctrl+G)
- [x] Zoom in/out (Ctrl+/-, buttons, editable entry)
- [x] Actual size (Ctrl+0), fit-width (Ctrl+1)
- [x] TOC sidebar (F9) with tree view
- [x] Fullscreen (F11)
- [x] Search bar (Ctrl+F)
- [x] Open file dialog (Ctrl+O) with MIME type filters
- [x] Per-document state persistence (page, scroll, zoom, rotation)
- [x] Live page number tracking in header bar
- [x] About dialog with app info and links
- [x] Desktop entry, AppStream metainfo, GSettings schema
- [x] App icon and logo
- [x] `--version` / `-v` flag
- [x] *Legacy:* Memory-bounded cache (50-page sliding window) — *Replaced in v1.2*

## Phase 1: The Engine Room (Memory & Velocity)
*Replacing the brute-force static cache with intelligent resource pacing. If it eats RAM here, the rest doesn't matter.*

- [x] **64MB MuPDF Clamp** — Hardcode `fz_new_context` limit to drop baseline RAM footprint.
- [x] **Velocity Tracker** — Implement `dy/dt` calculation via `gtk_widget_add_tick_callback`.
- [x] **High-Velocity Abort** — Ensure `FwCache` drops all queued jobs instantly when the user scrubs.
- [x] **Surgical Mutexing** — Move Cairo surface copying outside the MuPDF global lock.
- [x] **Thread Drip-Feeding** — Force the `GThreadPool` worker to evaluate velocity post-render.
- [x] **Two-Tier Cache Split** — Separate parsed backend objects (low RAM) from rendered cairo surfaces (high RAM).
- [x] **Ref-Count FwView Pointers** — Use `g_object_ref`/`unref` for `document` and `cache` to fix dangling pointers and prevent segfaults on document swap.
- [x] **DjVu Serialization Queue** — Implement thread-safe abort/queue management for `ddjvuapi` to prevent 100% CPU lockups during rapid scrolling.
- [x] **Fast DjVu Probing** — Implement a fast-path for DjVu page dimensions on open to kill startup latency for large scans.
- [x] **MuPDF Render Bottleneck** — Context-per-thread scaling bypasses the single `GMutex` serialization.

## Phase 2: The Rendering Pipeline
*Ensuring the pixels hit the glass cleanly on modern hardware.*

- [x] **Wayland Fractional Scaling** — Multiply MuPDF render resolution by the GTK widget scale factor. No blurry text on 150% scaling.
- [x] **Backend Parity** — Standardize DJVU & PDF interactions so the UI doesn't care what format is loaded.
- [x] **Invert Colors (Dark Mode)** — Bitwise NOT on RGB channels of rendered surfaces (Ctrl+I).
- [x] **Persistent Thumbnail Tier** — Lazy low-resolution previews cached for the life of the document, used as placeholders during fast scroll and zoom transitions (v1.5).
- [x] **Per-Frame Texture Cache** — Cache `GdkTexture` in `CacheEntry` so snapshot doesn't re-allocate wrappers every frame (v1.5).
- [x] **Hot-Path Pixmap Conversion** — Hoist branches and unroll the 4-pixel inner loop in the MuPDF → cairo pixel shuffle (v1.5).
- [x] **Scroll Velocity Cap** — Bound per-event scroll distance so sustained fast scrolling cannot outpace the render pipeline (v1.5).
- [x] **Zero-Copy MuPDF Render** — Use `fz_device_bgr` + `fz_new_pixmap_with_bbox_and_data` to render directly into the cairo ARGB32 buffer. No intermediate pixmap, no channel shuffle, no scalar loop (v1.6, technique from zathura-pdf-mupdf).

## Phase 3: Spatial Navigation
*How the user moves around the document. It must feel physical and precise.*

- [x] **Ctrl+Scroll Wheel Zoom** — Anchor zoom to the pointer coordinates, not the viewport center.
- [x] **Fit-Page & Fit-Width** — (Ctrl+1, Ctrl+2). Re-calculate bounding boxes on widget resize allocations.
- [x] **Rotation** — (Ctrl+Shift+Plus/Minus). 90-degree increments, invalidating and re-rendering the Cairo cache.
- [x] **Scroll Position Precision** — Restore sub-page scroll coordinates accurately across zoom level changes.

## Phase 4: Text & Geometry
*Extracting data from the render without locking the UI thread.*

- [x] **Text Selection** — Click-drag to select text using backend `get_text` with rectangle calculations.
- [x] **Selection Overlay** — Paint a semi-transparent blue GTK overlay over the selected region (avoid re-rendering the base PDF).
- [x] **Copy to Clipboard** — Pipe selected text directly to `GdkClipboard` (Ctrl+C).
- [x] **Dynamic Cursors** — I-beam over selectable text, pointer hand over link areas.

## Phase 5: Search & Illumination
*Finding data fast.*

- [x] **Async Search** — Pass search queries to a background worker. (v0.7)
- [x] **Search Result Highlighting** — Paint semi-transparent yellow overlays on matching text regions; active hit in orange. (v0.7)
- [x] **Search Navigation** — F3 / Shift+F3 to cycle through matches, automatically scrolling to the active match. (v0.7)
- [x] **Match Count** — "3 of 47" label in the search bar (with `+` while still scanning). (v0.7)

## Phase 6: Document Topology
*Moving through the structure and embedded contents of the file.*

- [x] **Sidebar TOC Highlight** — Track the current page and highlight the active section in the sidebar during scrolling. (v0.8)
- [x] **Sidebar Click Navigation** — Clicking a TOC entry jumps immediately to the target page. (shipped early; formally landed v0.8)
- [x] **Internal Link Jumps** — Clicking a linked footnote or index item navigates to the target page.
- [x] **Navigation Stack (History)** — Alt+Left / Alt+Right to go back/forward after jumping via links or TOC. (v0.8)

- [x] **Embedded File Extraction** — `Save Embedded Files…` menu entry walks the PDF /EmbeddedFiles xref via `pdf_is_embedded_file` + `pdf_get_filespec_params` + `pdf_load_embedded_file_contents` + `fz_save_buffer`; user picks an output folder and every attachment is saved under a sanitized basename (defeats `../path/traversal`). DjVu and CBR backends return NULL — neither format has an attachment mechanism. (v0.11)

## Phase 7: Format Expansion
*Leveraging MuPDF to open everything in the anti-library.*

- [x] **Comic Books (CBZ)** — Routed through the MuPDF backend; CBR best-effort with a libunrar caveat. (v0.9)
- [x] **CBR via libarchive** — New `FwDocumentCbr` backend at `src/fw-document-cbr.c` enumerates RAR/7z/tar image entries via libarchive, decodes via MuPDF's image API, paints into cairo using the v0.7 zero-copy draw-device pattern. Single-mutex per archive (libarchive isn't thread-safe per-reader); the velocity engine + thumbnail tier hide the serial cost. (v0.11)
- [x] **XPS / EPUB / FB2 / MOBI Support** — Factory dispatches all of these to the MuPDF backend; reflowable formats (EPUB / FB2 / MOBI) get an `fz_layout_document(600, 900, 11)` pass on every render-instance open. EPUB pagination is "wherever MuPDF's default layout puts it" — Foliate handles serious EPUB reading better. (v0.11)
- [x] **External Links** — Open web URLs in default browser via `GtkUriLauncher`.

## Phase 8: Desktop Symbiosis
*Making it a native, well-behaved GTK citizen.*

- [x] **GtkListView Migration** — `fw-sidebar.c` rewritten against `GtkListView` + `GtkTreeListModel` + `GtkSingleSelection`; new `FwTocItem` GObject replaces the `GtkTreeStore` row data; current-page highlight walks the `FwTocItem` tree, expands ancestor `GtkTreeListRow`s, then walks the flat model to find the position to select. (v0.11)
- [x] **Empty Window State** — `AdwStatusPage` with "Open File…" button when no document is loaded; crossfades to the document view on open. (v0.10)
- [x] **Drag-and-Drop** — `GtkDropTarget` accepting `G_TYPE_FILE` on the window; routes through `fw_window_open_file`. (v0.10)
- [x] **Printing** — `GtkPrintOperation` wired to `Ctrl+P`; renders each page via `fw_document_render_page` at the print context's DPI capped at 300. (v0.10)

## Phase 9: Session Resilience
*Bulletproofing the state tracker.*

- [x] **LRU Eviction** — Cap `state.json` at 500 entries. Evict the oldest document state so the config file doesn't bloat.
- [ ] **Document Properties Dialog** — Display metadata (title, author, page count, file size).
- [ ] **Keyboard Shortcuts Dialog** — Implement `GtkShortcutsWindow` showing all bindings (Ctrl+?).

## Phase 10: The 1.0 Release (Concrete)
*Getting it out the door.*

- [ ] **Application Assets** — Finalize SVG icon and `dev.hermitage.Hermitage.desktop` file.
- [ ] **AppStream Metadata** — Write the XML for software centers.
- [ ] **Flatpak Manifest** — Build the GNOME 50 Flatpak. Test strict container compilation for MuPDF/DjVuLibre.
- [ ] **Permissions Audit** — Lock down the Flatpak sandbox. File access only via portal, no unnecessary network access.
- [ ] **Tag 1.0.0 & Release**

## Phase 11: Reference-Repo Borrows
*Wins identified by surveying SumatraPDF, Zathura, zathura-pdf-mupdf, Sioyek, Plato, and the canonical mupdf-gl viewer. Each item names **the source code to study** in the reference repo and **the Framework target** where the change lands (see `CLAUDE.md` "Reference repos" for the per-repo file map and license matrix). Ranked by ROI, not chronology — promote items into earlier phases when they ship. Line numbers are accurate at the time of writing; verify with `grep` if drifted.*

### Tier 1 — High-impact / pre-1.0 candidates

- [ ] **`fz_cookie` in-flight render cancellation** — borrow Sumatra's per-job `fz_cookie*` so MuPDF aborts mid-`fz_run_page` instead of running to completion when the user has already scrolled away. Currently jobs only check cancellation at *start* (`fw-cache.c:267`); on a 50 MB scanned PDF this saves 1–3 seconds per stale page. Neither zathura nor sioyek does this — Framework would lead.
  - **Study:** sumatra `.sumatrapdf/src/EngineMupdf.cpp:3178` (`FitzAbortCookie`, `fzcookie` plumbed into `RenderPage`); mupdf `.sumatrapdf/mupdf/include/mupdf/fitz/device.h:479` (`fz_cookie` struct definition — write `cookie->abort = 1` from any thread).
  - **Target:** add `fz_cookie *cookie` field to `RenderJob` in `src/fw-cache.c` (struct around line 44); thread cookie pointer through `fw_document_render_page_from_handle` in `src/fw-document.h:104` and `src/fw-document-pdf.c:render_page_direct` (line 88, pass to `fz_run_page` line 131); set `cookie->abort` from `fw_cache_set_velocity` (line 909) on SCRUBBING transition and from `fw_document_cancel_render`.

- [ ] **Cached `fz_stext_page` per `ParsedEntry`** — when the cache opens a page handle, also build the structured-text page once and stash it. Reused by both selection and search. Eliminates re-extraction on every interaction.
  - **Study:** zathura-pdf-mupdf `.zathura-mupdf/zathura-pdf-mupdf/page.c:38` (`mupdf_page_t.text` cached at `pdf_page_init` time); sioyek `.sioyek/pdf_viewer/pdf_renderer.cpp:396` (search-side reuse via `fz_new_stext_page_from_page_number`).
  - **Target:** extend `ParsedEntry` in `src/fw-cache.c` (struct near line 39) with an `fz_stext_page *stext` field; populate it in `render_worker` after `fw_document_open_page` (line 298); rewrite `pdf_get_text` in `src/fw-document-pdf.c` (around line 495) and the search/select paths to consume the cached stext rather than re-extracting.

- [ ] **GThreadPool sort-function priority** — replace the manual `submit_next_jobs` loop with `g_thread_pool_set_sort_function`; workers naturally pick the most-recently-viewed page first. Drops the throttling code in `fw_cache_set_priority`.
  - **Study:** zathura `.zathura/zathura/render.c:94` (`g_thread_pool_set_sort_function` setup) and `render.c:927` (`render_thread_sort` comparator using `last_view_time`). Pure GLib, drops in cleanly.
  - **Target:** rewrite `submit_next_jobs` in `src/fw-cache.c` (line 385) so it pushes *all* in-window jobs at once and lets the pool sort; delete or simplify `priority_order` (line 84) and the throttling at `fw_cache_set_priority:543`. Add `last_view_time` to `RenderJob` and bump it from `fw_cache_set_priority` instead of rebuilding the order array.

- [ ] **Bytes-aware cache cap** — replace `CACHE_WINDOW = 30` with a byte budget (~512 MB) tracking `stride * height` per surface. A 50-page comic at fit-page is 10 MB; a 30-page CAD drawing at fit-width is 1.5 GB. Page-count caps mis-fit by 2 orders of magnitude.
  - **Study:** sumatra `.sumatrapdf/src/RenderCache.h:16` (`MAX_BITMAPS_CACHED`) and `RenderCache.cpp:178` (`FreeIfFull` eviction logic); plus our own measurements on Calibre Library samples.
  - **Target:** delete `CACHE_WINDOW` from `src/fw-cache.c:180`; track total cached bytes on every `CacheEntry` insert/evict in `cache_entry_free` (line 99) and the worker store path (around line 339); change the eviction loop in `fw_cache_set_priority:651` to drop bytes-cap victims by surface size rather than window membership.

- [ ] **Async, progressive search** — already in Phase 5 as TODO. Implementation: dedicated `GTask` worker, scans page-by-page using the cached stext page from above, emits hits via signals every N pages. Sumatra opportunistically extracts text *during render* so visited pages are already pre-warmed by the time the user opens search — adopt that too.
  - **Study:** sioyek `.sioyek/pdf_viewer/pdf_renderer.cpp:342` (`run_search` — dedicated thread, emits `search_advance` every 16 pages); sumatra `.sumatrapdf/src/RenderCache.cpp:790` (`engine->GetTextForPage(req.pageNo)` called inside `RenderCacheThread` between renders).
  - **Target:** rewrite `fw_search_find` in `src/fw-search.c:40` (currently a synchronous loop blocking the UI) as a `GTask` that emits incremental "page-results" signals; have `render_worker` in `src/fw-cache.c:render_worker` (line 254) optionally extract stext after a successful render to pre-warm.

### Tier 2 — Medium-impact / post-1.0

- [ ] **`try_closest_rendered_page` zoom transition** — generalize Framework's `prev_texture` (one previous zoom only) to "find the nearest-zoom rendered surface and scale-transform it for display while the exact zoom renders." Eliminates the grey-flash during continuous Ctrl+scroll zoom.
  - **Study:** sioyek `.sioyek/pdf_viewer/pdf_renderer.cpp:236` (`try_closest_rendered_page` walks all cached responses for a `(path, page, slice)` tuple, picks min zoom-diff, lets the GPU rescale).
  - **Target:** extend `CacheEntry` in `src/fw-cache.c` to keep multiple `(zoom → surface/texture)` pairs (replacing the single `prev_*` slot at lines 13–17); rewrite `fw_cache_get_texture` (line 778) to walk the bag and return the nearest zoom; have `fw-view.c` snapshot pass apply a scale transform when zoom mismatches.

- [ ] **Tile/slice rendering as high-zoom fallback** — at zoom levels where `w*h*4 > 64 MB`, render in N×M slices instead of one cairo surface. Don't replace page-level rendering wholesale; add as fallback only. Without this, 600% zoom on an A0 poster allocates ~384 MB for one page.
  - **Study:** sumatra `.sumatrapdf/src/RenderCache.h:22` (`TilePosition` res/row/col) and `RenderCache.cpp:389` (`GetTileRes` decides tile resolution from page-vs-viewport ratio); sioyek per-request `num_h_slices/num_v_slices` in `.sioyek/pdf_viewer/pdf_renderer.cpp:67`.
  - **Target:** add a slice path in `src/fw-document-pdf.c:render_page_direct` (line 88) that takes `(slice_idx, n_h, n_v)` and renders one tile; cache key in `src/fw-cache.c` becomes `(page, slice)` — only used when `pixel_w * pixel_h * 4 > THRESHOLD`.

- [ ] **TTL + LRU hybrid cache eviction** — alongside the bytes cap, touch `last_access_time` on every `fw_cache_get_texture`; evict bytes-cap victims by oldest-access. Optionally drop anything older than 60s outside the visible window.
  - **Study:** sioyek `.sioyek/pdf_viewer/pdf_renderer.cpp:269` (`delete_old_pages` + 1Hz `garbage_collect_timer` at line 30); zathura `.zathura/zathura/render.c:977` (`page_cache_lru_invalidate` walks by `last_view_time`).
  - **Target:** add `gint64 last_access_time` to `CacheEntry` in `src/fw-cache.c` (struct around line 13); update it inside `fw_cache_get_texture` (line 778) and `fw_cache_get_page` (line 731); add a `g_timeout_add` 1 Hz GC that runs the bytes-cap eviction logic.

- [ ] **Hue-preserving recolor (smarter "Invert Colors")** — current Ctrl+I is bitwise NOT on RGB, which destroys diagram color cues (red lines become cyan, signal traces lose meaning). Zathura preserves hue and remaps lightness between configurable light/dark colors via the `colorumax` HSL pipeline — a red arrow stays red on a dark background.
  - **Study:** zathura `.zathura/zathura/render.c:280` (`zathura_renderer_set_recolor_colors`) and `render.c:495+` (`colorumax` HSL transform, hue/lightness math).
  - **Target:** replace the bitwise-invert color matrix in `src/fw-view.c:325` (`graphene_matrix_init_from_float`) with a saturation/lightness-aware matrix derived from configurable light/dark colors; expose `recolor-light` / `recolor-dark` in the GSettings schema (`data/com.github.vrnvctss.framework.gschema.xml`).

- [ ] **Word selection on double-click, line on triple-click** — Sumatra's `FindClosestGlyph` walks the cached glyph-rect array to find the glyph nearest the click point (preferring glyphs the cursor is *over*); `SelectWordAt` extends the selection to whitespace boundaries. Combined with cached `fz_stext_page` from Tier 1, this is a straightforward UX win — currently click-drag is the only way to select text.
  - **Study:** sumatra `.sumatrapdf/src/TextSelection.cpp:44` (`FindClosestGlyph` distance-to-glyph-center with "prefer glyphs the cursor is over") and `TextSelection.h:28` (`SelectWordAt`).
  - **Target:** add a `GtkGestureClick` alongside the existing drag gestures in `src/fw-view.c:486` (`on_drag_begin`); on `n_press == 2`, call a new `fw_document_select_word` interface method backed by walking the cached `fz_stext_page` (depends on Tier 1's stext caching).

- [ ] **Per-instance MuPDF store size scaling** — currently `fz_new_context (NULL, NULL, 64 << 20)` ×8 = 512 MB worst-case just for stores. Plato uses 32 MB; Sumatra uses `FZ_STORE_DEFAULT`; sioyek lets MuPDF pick. Make this an opened-document property scaled by file size, not a magic constant.
  - **Study:** plato `.plato/crates/core/src/document/mupdf_sys.rs:37` (`CACHE_SIZE = 32 * 1024 * 1024`); sumatra `.sumatrapdf/src/EngineMupdf.cpp:1846` (`fz_new_context(nullptr, &fz_locks_ctx, FZ_STORE_DEFAULT)`).
  - **Target:** the two `fz_new_context (NULL, NULL, 64 << 20)` calls in `src/fw-document-pdf.c:175` (main context) and `src/fw-document-pdf.c:224` (per-instance render context inside the loop). Compute store size from `g_stat` of the input file before opening.

### Tier 3 — Investigation / measurement before commit

- [ ] **Reconsider 8× `fz_open_document` model** — Sumatra and Sioyek share one parent `fz_context` and use `fz_clone_context` per render thread, sharing font/image stores. Framework's 8-independent-document model is simpler but ~8× memory. On large textbooks with embedded TrueType + JPEG2000 streams this likely OOMs on low-RAM laptops.
  - **Study:** sumatra `.sumatrapdf/src/EngineMupdf.cpp:1700` (`fz_lock_context_cs`/`fz_unlock_context_cs` callbacks), `EngineMupdf.cpp:1766` (`GetOrClonePerThreadContext`), `EngineMupdf.cpp:1843` (`fz_locks_ctx` setup before `fz_new_context`); sioyek `.sioyek/pdf_viewer/pdf_renderer.cpp:42` (`init_context = fz_clone_context`) and `pdf_renderer.cpp:454` (`get_document_with_path` keyed by `(thread_index, path)`).
  - **Target:** the `RenderInstance` array and rationale comment in `src/fw-document-pdf.c:21–57` (`MAX_RENDER_INSTANCES = 8`, the `Cloned contexts share a font/image store ...` comment, and per-instance state); `pdf_open` open-loop at line 223; `pdf_render_page` slot selection at line 346. Build a feature-flagged `FwDocumentPdfClone` backend, measure RSS on worst-case Calibre samples, decide. Verify the comment at `fw-document-pdf.c:24` against MuPDF 1.24+ before committing.

- [ ] **Per-thread-shared-context hybrid** — sioyek's middle-ground model (one parent context, per-thread `fz_clone_context`, per-(thread, path) `fz_document`). Measure if this beats both pure Sumatra and Framework's current model on the textbook benchmark before picking.
  - **Study:** sioyek `.sioyek/pdf_viewer/pdf_renderer.cpp:41–63` (`init_context`, `start_threads`, `run` thread main with cloned context); sioyek `pdf_renderer.cpp:454` (per-`(thread_index, path)` document map) — this is the full pattern.
  - **Target:** alternative implementation of `src/fw-document-pdf.c` rendering layer; same insertion points as the previous item.

## Phase 12: Stress-Testing & Debugging Suite
*Until we can break it on demand, every release is a guess. Build the harness that proves "still fast" and "still doesn't leak" before the borrows in Phase 11 land. Every item lives under `tests/` (currently doesn't exist) and is gated by a meson option so packagers don't pay the cost.*

### 12.1 Test infrastructure

- [ ] **`tests/` tree + `meson_options.txt`** — add `-Dstress=true` (default false) to opt-in to building the harness. New top-level `tests/meson.build` with subdirs `tests/stress/`, `tests/bench/`, `tests/scripts/`. Tests link against the same static archive of `framework_sources` so internal symbols (`fw_cache_*`, `render_page_direct`, etc.) are reachable without exposing them publicly.
- [ ] **Corpus manifest** — `tests/corpus.json` holding canonical sample paths plus per-sample tags (`large`, `scanned`, `djvu`, `poster`, `cad`, `bookmarks`). Default corpus = Brandon's `/home/bdkl/docs/Calibre Library/`. Manifest read by all stress tools so a single source of truth controls what's exercised. Honour `FW_TEST_CORPUS_ROOT` env var override for CI without Calibre.

### 12.2 Stress tests (the "can it survive abuse" set)

- [ ] **`stress-scrub`** — opens a document headlessly (no widget tree, just `FwDocument` + `FwCache` driven by direct `fw_cache_set_priority` + `fw_cache_set_velocity` calls), simulates a worst-case scrub: 0→last page in 500 ms, then 5×back-and-forth, then a settle. Asserts: zero crashes, RSS stays under a configurable cap (default 600 MB), no surface memory leak after settle (peak vs. resting RSS within 5%). Failure prints a `coredumpctl debug` invocation. Borrows the dispatch model from `.zathura/zathura/render.c` for the velocity injector.
- [ ] **`stress-zoom-storm`** — page 0 → 50 alternating Ctrl+Plus / Ctrl+Minus across the full zoom range (10%–1000%). Confirms `prev_texture` slot doesn't double-pin surfaces; counts `pixmap → cairo_surface_t` allocations vs. frees via a debug counter exported when `-Dstress=true`. Catches the bug class where a zoom-storm leaks one surface per cycle.
- [ ] **`stress-multidoc`** — open/close 50 documents in sequence (and another mode: 10 in parallel `FwDocument` instances) to exercise the dispose path without the windowing layer. Validates the cache leak fix from v1.3.3 doesn't regress, and that the 8-instance MuPDF backend tears down cleanly under churn.
- [ ] **`stress-corpus-soak`** — iterate the entire corpus, render every page at fit-width once, twice with rotation, scrub each. The "hit every code path on every kind of file we have" run. Configurable with `--max-time` for CI, default unbounded for local. Reports per-file timings and surfaces any `fz_catch` warnings.

### 12.3 Benchmarks (the "is it still fast" set)

- [ ] **`bench-render`** — measures cold/warm `render_page_direct` wall time across the corpus, p50/p95/p99 buckets per backend (PDF/DjVu) and per-tag (large textbook, comic, scanned book). Output is JSON to `tests/bench/results-<commit>.json`; a `tests/scripts/bench-diff.py` compares two runs and flags regressions over a configurable threshold (default 5%).
- [ ] **`bench-cache-hit-rate`** — drives the cache through a recorded scroll trace and reports hit/miss ratio per state (STATIC/CRUISING/SCRUBBING). Trace files live in `tests/traces/*.scroll` (binary, replayable). Useful for tuning the bytes-aware cache cap (Phase 11 Tier 1) without manual A/B testing.
- [ ] **`bench-startup`** — open-to-first-paint latency for representative samples. Catches regressions in the `apply_fit_width_tick` deferred layout path.

### 12.4 Debugging setup

- [ ] **Sanitizer build option** — `-Dsanitize=address,undefined` in `meson_options.txt` flips on `-fsanitize=address,undefined,leak`. Per-file `LSAN_SUPPRESSIONS` for known MuPDF/DjVu leaks (font-store statics, ddjvuapi message buffers) in `tests/lsan.supp`. Document the flow in `CLAUDE.md`.
- [ ] **`tests/scripts/debug.sh`** — wraps `gdb -batch` with the canonical batch flags used in `CLAUDE.md`, plus pre-loaded breakpoints in `tests/scripts/framework.gdb` for `fz_catch` (the silent-warning trap), `cache_entry_free`, `submit_next_jobs`, and `render_worker`. One command for "open this file under gdb with all the right hooks."
- [ ] **`tests/scripts/coredump-triage.sh`** — given a coredump file or PID, runs `coredumpctl debug` non-interactively, captures `thread apply all bt`, MuPDF context state from each `RenderInstance`, and the cache state via a custom `gdb` pretty-printer for `FwCache`. Output goes to `~/.local/share/framework/triage/<timestamp>/`.
- [ ] **Trace-domain matrix** — `tests/scripts/trace-replay.sh <log>` parses `FW_DEBUG=1` output and produces a timeline (cache state transitions, render queue depth, eviction events) as a single SVG. Catches "why did it freeze" without manual log reading. Lightweight: just awk + svgwrite.
- [ ] **`framework --self-test`** — built only with `-Dstress=true`, runs a 5-second smoke test: opens a known sample, scrubs through it, validates a hash of the rendered first-page surface against a baseline. Used by the Flatpak CI pipeline (Phase 10) to catch toolchain regressions before shipping.

## Phase 13: Layout & Reflow Architectures
*Structural shifts for specific formats beyond the single-canvas raster paradigm. Focus on rich aesthetics and format-specific native behaviors.*

- [ ] **Fractal-Style EPUB Reflow** — Bypass MuPDF's fixed-layout engine for EPUBs (and reflowables). Parse the XHTML/DOM and map structural blocks into a `GListModel`, rendered via `GtkListView` using native GTK widgets (`GtkLabel` with Pango). This provides true reflow on window resize, native text selection, drops heavy Cairo surface caching, and achieves a gorgeous, modern aesthetic identical to Fractal's list architecture.
- [ ] **Comic Facing Pages (Left/Right)** — For CBR/CBZ archives, implement a two-up facing pages layout (left/right). Requires adjusting the `FwView` geometry math and cache eviction window to handle two active `RenderJob` instances side-by-side, preserving aspect ratios and handling mismatched page dimensions gracefully.
- [ ] **Manga Mode (Right-to-Left)** — Borrowed from MComix/Komikku. Invert keyboard navigation (Right Arrow = previous page, Left Arrow = next page) and default the horizontal scrollbar to the right edge. Crucial for Japanese comics.
- [ ] **Webtoon Mode (Infinite Canvas)** — Borrowed from Komikku. A specialized continuous vertical view where the Y-gap between pages is exactly `0px`, stitching long-strip comics into a seamless vertical scroll.

## Phase 14: Best-in-Class UX (The "Sumatra Clone" Polish)
*Features identified across reference repositories that elevate the viewer from 'functional' to 'exceptional'. Translated into native GTK/GNOME idioms.*

- [ ] **Smart Text Selection (from SumatraPDF)** — Sumatra's `FindClosestGlyph` handles selection intuitively by preferring the glyph the cursor is actually hovering over. We will implement double-click-to-select-word and triple-click-to-select-line.
  - **Study:** sumatra `.sumatrapdf/src/TextSelection.cpp:44` (`FindClosestGlyph`).
  - *GTK Implementation:* Add a `GtkGestureClick` to `FwView` reacting to `n_press == 2` and `n_press == 3`. This will query the cached `fz_stext_page` (from Phase 11) to find whitespace boundaries and instantly apply the selection overlay without requiring a click-drag.
- [ ] **Seamless Auto-Reload (from SumatraPDF / Zathura)** — Sumatra is famous among LaTeX/Typst users because it never locks the PDF file and reloads it instantly when recompiled.
  - **Study:** sumatra `.sumatrapdf/src/utils/FileWatcher.cpp` and zathura `.zathura/zathura/file-monitor-glib.c`.
  - *GTK Implementation:* Attach a `GFileMonitor` to the active document. On the `G_FILE_MONITOR_EVENT_CHANGED` signal, transparently swap the `FwDocument` backend instance, invalidate the `FwCache`, restore the exact `scroll_position` from `FwState`, and display an `AdwToast` ("Document updated") to the user.
- [ ] **Margin Cropping (from Plato / Sioyek)** — E-ink readers and advanced viewers often auto-crop white margins to maximize text size on small laptop screens.
  - **Study:** sioyek `.sioyek/scripts/dual_panelify.py` (crop algorithms).
  - *GTK Implementation:* Add a "Crop Margins" toggle. When active, scan the first few pages using MuPDF to calculate a unified content bounding box. Apply a translation matrix during the `fw_view_snapshot` phase to hide the whitespace margins globally.
- [ ] **Visual Ruler / Reading Mark (from Sioyek)** — Sioyek features a "visual mark" that darkens the page except for the active reading line to help users keep their place in dense technical PDFs.
  - **Study:** sioyek `.sioyek/pdf_viewer/main_widget.cpp` (visual mark overlay logic).
  - *GTK Implementation:* A toggleable mode in `FwView` that appends a semi-transparent black `GskColorNode` over the whole screen, with a masked-out horizontal band that tracks the mouse Y-coordinate.
- [ ] **Interactive Loupe / Magnifying Glass (from YACReader)** — Useful for dense comic panels or small text where global zoom is too disruptive.
  - **Study:** yacreader `.yacreader/YACReader/magnifying_glass.cpp` and `magnifying_glass.h`.
  - *GTK Implementation:* Render a secondary `GtkSnapshot` clipped to a circular mask centered on the cursor, scaling the underlying Cairo surface texture by a user-defined multiplier (e.g., 2.5x).

### Explicitly NOT borrowing

- **Sumatra**: tile-as-default rendering (only as fallback per Tier 2); Win32-anything (`UpdateBitmapColors`, `CRITICAL_SECTION`, `HBITMAP`, GDI paint paths); `Func1<>` callback templates; `EngineEbook`/`EngineChm`/`EngineImages`/`EnginePs` (out of scope per spec §13).
- **Zathura**: girara UI; vim keybindings; per-page widget model (our single-canvas paint is a deliberate choice); `epub.css` user stylesheets.
- **Sioyek**: Qt5/QML stack; OpenGL textures (we use `GdkTexture` + `gtk_snapshot_append_texture`); PhD-research features (custom highlights, ruler mode, portal-style links).
- **Plato**: e-ink-specific UI; `framebuffer/` direct-to-fb rendering; single-threaded `Rc<Context>` model (loses our parallelism).
- **mupdf-gl**: single-threaded design; in-tree custom UI toolkit.

---

## Deferred (v2.0+ / Strictly Out of Scope for 1.0)
*Listed so the architecture doesn't preclude them, but forbidden from development until 1.0 ships.*

- [ ] Single page view mode
- [ ] Facing pages view mode (PDFs/general docs — see Phase 13 for Comic facing pages)
- [ ] Thumbnail sidebar (alternative to TOC)
- [ ] Annotations (highlight, underline — stored externally)
- [ ] Presentation mode (page-at-a-time, no chrome)
- [ ] Smooth pinch-to-zoom on touchscreens
- [ ] Configurable keybindings via GSettings
is a deliberate choice); `epub.css` user stylesheets.
- **Sioyek**: Qt5/QML stack; OpenGL textures (we use `GdkTexture` + `gtk_snapshot_append_texture`); PhD-research features (custom highlights, ruler mode, portal-style links).
- **Plato**: e-ink-specific UI; `framebuffer/` direct-to-fb rendering; single-threaded `Rc<Context>` model (loses our parallelism).
- **mupdf-gl**: single-threaded design; in-tree custom UI toolkit.

---

## Deferred (v2.0+ / Strictly Out of Scope for 1.0)
*Listed so the architecture doesn't preclude them, but forbidden from development until 1.0 ships.*

- [ ] Single page view mode
- [ ] Facing pages view mode
- [ ] Thumbnail sidebar (alternative to TOC)
- [ ] Annotations (highlight, underline — stored externally)
- [ ] Presentation mode (page-at-a-time, no chrome)
- [ ] Smooth pinch-to-zoom on touchscreens
- [ ] Configurable keybindings via GSettings
