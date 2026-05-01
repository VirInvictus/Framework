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
- [x] **Document Properties Dialog** — `AdwDialog` driven by a new `get_metadata` interface method. PDF backend extracts via `fz_lookup_metadata` (Title/Author/Subject/Keywords/Creator/Producer/CreationDate/ModDate + format/encryption); DjVu and CBR fall back to filename, size (`g_format_size`), pages, and extension-derived format. PDF date strings (`D:YYYYMMDD…`) are parsed to human-readable form. (v0.12)
- [x] **Keyboard Shortcuts Dialog** — Custom `AdwDialog` containing `AdwPreferencesPage` groups (File / Navigation / Zoom & Rotation / Search / View / Selection) with `GtkShortcutLabel` rows. Wired to `win.show-help-overlay` (the conventional GTK action), bound to `Ctrl+?` and `F1`. Avoids `GtkShortcutsWindow` (deprecated in GTK 4.18). (v0.12)

## Phase 11: Reference-Repo Borrows
*Wins identified by surveying SumatraPDF, Zathura, zathura-pdf-mupdf, Sioyek, Plato, and the canonical mupdf-gl viewer. Each item names **the source code to study** in the reference repo and **the Framework target** where the change lands (see `CLAUDE.md` "Reference repos" for the per-repo file map and license matrix). Ranked by ROI, not chronology — promote items into earlier phases when they ship. Line numbers are accurate at the time of writing; verify with `grep` if drifted.*

### Tier 1 — High-impact / pre-1.0 candidates

- [x] **`fz_cookie` in-flight render cancellation** — Implemented as a PDF-backend-internal feature rather than threading the cookie through the public `FwDocument` interface (DjVu and CBR have their own cancel mechanisms with different semantics). Each `pdf_render_page` allocates an `fz_cookie` on its worker stack and publishes the pointer in `FwDocumentPdf::active_cookies[MAX_RENDER_INSTANCES]` under a *separate* `cookies_lock` mutex — distinct from the per-instance render lock so cancel never blocks on a worker mid-`fz_run_page`. New `pdf_cancel_render` walks the array and writes `abort = 1`; MuPDF sees the flag at its next checkpoint and bails. `render_page_direct` discards the partial surface when `cookie->abort` was set. The PDF backend previously had no `cancel_render` implementation, so this also closes that gap. Verified ASan-clean. (v0.17)

- [x] **Cached `fz_stext_page` per page** — Implemented as a PDF-backend-internal cache (`FwDocumentPdf::stext_cache`, GHashTable<int, fz_stext_page*>) rather than threaded through `ParsedEntry` — the PDF backend doesn't use the parsed-handle cache (each render uses one of 8 independent fz_documents), so there's no `ParsedEntry` to extend for it. All entries owned by `self->ctx`; populated lazily on first `pdf_get_text` / `pdf_search` call per page. `pdf_search` switched from `fz_search_page` to `fz_search_stext_page` to consume the cached entry. Verified clean under ASan, with a new `stress-search-cache` test asserting ≥1.5× warm/cold speedup (observed: 6.85× on Effective Java, 332 ms cold → 48 ms warm). (v0.18)
  - **Open follow-up:** opportunistic render-time pre-warm (Sumatra's `RenderCache.cpp:790` pattern) — extract stext from the rendered fz_page during render-worker success path. Would shift the first-search latency off the search path entirely. Skipped in v0.18 because it requires using the per-instance `fz_context` (since rendering uses inst→ctx), which means cleanup needs per-instance tracking. Not hard, just a separate concern. (open)

- [x] **GThreadPool sort-function priority** — `RenderJob` carries `last_view_time`; the pool runs `g_thread_pool_set_sort_function (render_job_compare)`; `submit_next_jobs` pushes all unrendered in-window pages at once and lets the pool reorder by recency. The asymmetric "near vs far" priority tiers and the `active_jobs < job_limit` throttle are gone. Pattern from zathura `.zathura/zathura/render.c:94`. (v0.14)

- [x] **Symmetric ±10-page parsed window with toggleable kinetic scrolling** — Priority window is now visible + 10 forward + 10 backward, interleaved, in all non-SCRUBBING states. A capture-phase `GtkEventControllerScroll` on `FwView` caps each scroll event at 90 px (wheel ticks scaled via `SCROLL_WHEEL_STEP = 60` first), applied directly to the vadjustment; this replaces GTK's default kinetic momentum scrolling. The cap is gated by a new GSettings key `kinetic-scrolling` (bool, default false) wired to a primary-menu toggle "Kinetic Scrolling" via `g_settings_create_action` — flipping the menu item switches behavior live. Default off = cache-friendly (the new project default); on = momentum flick for "gliding through research/school." (v0.14)

- [x] **Startup blur on saved-state open (regression)** — Real fix, not the predicted "obviated for free." Root cause was `update_cache_priority` in `fw-view.c` bailing on `gtk_widget_get_height <= 0` during state-restore (the adjustment value-changed fires before allocation settles). Fix adds a fallback path: when widget height is unallocated, derive the visible page from `page_y_offsets[]` and push that single page as priority. Combined with the sort-function dispatch above, the saved-page job lands ahead of the initial pages 0–13 queue and renders before the window first paints. (v0.14)

- [x] **Bytes-aware cache cap** — `CACHE_WINDOW = 30` replaced by `total_cached_bytes` tracking + `byte_cap` (512 MB default, `FW_CACHE_BYTES_CAP_MB` env override). Accounting is live at every surface store/replace/evict path under the existing mutex; `cache_entry_free` is documented as not auto-decrementing (callers handle it). Eviction fires only when over cap and prefers outside-priority pages (visible + ±10 are never evicted). The new policy keeps outside-priority surfaces in cache until cap is hit, so scroll-back is instant when there's headroom — the v1.3.3 "drop everything outside window" behavior is gone. Verified across all six backends (PDF/DjVu/EPUB/MOBI/CBZ/CBR), and the `stress-scrub` test pins a tight 128 MB cap to exercise the eviction path during Phase 4 (slow walk). Under ASan: clean. Pattern from sumatra `.sumatrapdf/src/RenderCache.cpp` `FreeIfFull` translated to GLib idioms. (v0.16)

### Tier 2 — Medium-impact / post-1.0

- [x] **`try_closest_rendered_page` zoom transition** — `CacheEntry` now retains up to `MAX_PREV_ZOOM_SLOTS = 3` previous-zoom snapshots in a `ZoomSlot[]` array (each carrying `zoom`/`rotation`/`scale_factor`/`size_bytes`). `fw_cache_start` demotes the current surface into `prev_slots[0]`, shifts older slots right, evicts the oldest on overflow. `fw_cache_get_texture` returns the slot with minimal `|zoom − target|` (matching rotation+scale) when the current-gen surface isn't ready — GTK's `gtk_snapshot_append_texture` already auto-scales, so the view code didn't need a transform change. Bytes accounted in `total_cached_bytes` so the existing 512 MB byte_cap still bounds RAM. ASan-clean; `stress-zoom-storm`'s settled-RSS cap was raised 1024 → 1280 MB to accommodate the legitimate extra cache. (v0.28)

- [ ] **Tile/slice rendering as high-zoom fallback** — at zoom levels where `w*h*4 > 64 MB`, render in N×M slices instead of one cairo surface. Don't replace page-level rendering wholesale; add as fallback only. Without this, 600% zoom on an A0 poster allocates ~384 MB for one page.
  - **Study:** sumatra `.sumatrapdf/src/RenderCache.h:22` (`TilePosition` res/row/col) and `RenderCache.cpp:389` (`GetTileRes` decides tile resolution from page-vs-viewport ratio); sioyek per-request `num_h_slices/num_v_slices` in `.sioyek/pdf_viewer/pdf_renderer.cpp:67`.
  - **Target:** add a slice path in `src/fw-document-pdf.c:render_page_direct` (line 88) that takes `(slice_idx, n_h, n_v)` and renders one tile; cache key in `src/fw-cache.c` becomes `(page, slice)` — only used when `pixel_w * pixel_h * 4 > THRESHOLD`.

- [x] **TTL + LRU hybrid cache eviction** — `last_access_us` field added to `CacheEntry`; bumped on every `fw_cache_get_page` / `fw_cache_get_texture` hit and on worker-store success. Bytes-cap eviction loop now sorts outside-priority candidates oldest-first via a fileop-scope `LruVictim` GArray. The 1 Hz `g_timeout_add` GC layer is intentionally skipped — eviction-on-overflow already drops cold pages, and a separate idle GC would race the workers without measurable benefit. (v0.26)

- [x] **Hue-preserving recolor (smarter "Invert Colors")** — Replaces the bitwise-NOT 4×4 with a luminance-aware affine: compute BT.601 luma `Y` and add `(1−2Y)` to every channel, preserving each pixel's chromatic offset (R−Y, G−Y, B−Y). Red stays red, blue stays blue, but the lightness axis flips. Single `gtk_snapshot_push_color_matrix`, GPU-side, GSK clamps gamut for free. Configurable `recolor-light` / `recolor-dark` GSettings keys (the full zathura-style customization) remain a follow-up — current implementation hardcodes the standard white↔black mapping. (v0.22)

- [x] **Per-instance MuPDF store size scaling** — `pdf_pick_store_size(path)` helper picks 16/32/64/128 MB based on `g_stat` of the file (under 5 / 20 / 100 MB / above). Both `fz_new_context` call sites — main + per-instance render context — use the scaled value. Worst-case is now 1 GB across all 8 contexts on heavy textbooks (down from "uniform 32 MB regardless of doc size", which both wasted RAM on novels and pinched on PDF/A-3 textbooks with embedded JPEG2000 + TrueType). bench-render p50 cold/warm confirms ~3× store-hit speedup. (v0.26)

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

- [x] **`tests/` tree + `meson_options.txt`** — `-Dstress=true` (default false) gates the harness build. Top-level `meson_options.txt` and `tests/meson.build` exist; `tests/stress/` is populated. The src layer was refactored into a `framework-core` static library so internal symbols (`fw_cache_*`, `fw_document_*`, etc.) are reachable to tests via `framework_lib_dep` without exposing them publicly. (v0.15)
- [x] **Corpus manifest** — `tests/corpus.json` holds the canonical sample list (default root `/home/bdkl/docs/Calibre Library/`, env override `FW_TEST_CORPUS_ROOT`). Tagged samples (`large`, `textbook`, `djvu`, `scanned`, `reflow`). Currently consumed by `stress-scrub` via argv; the manifest reader for fully manifest-driven tests lands as bench/soak tests come in. (v0.15)

### 12.2 Stress tests (the "can it survive abuse" set)

- [x] **`stress-scrub`** — opens a document headlessly via `FwDocument` + `FwCache` (no widget tree; `view_widget = NULL`). Simulates worst-case scrub: 0 → last page in 500 ms (50 steps × 10 ms), 5 × back-and-forth, 3 s settle. Asserts no crashes, peak RSS under `FW_STRESS_RSS_CAP_MB` (default 800 MB), workers drain to idle. **Verified clean across all six backends** — PDF, DjVu, EPUB, MOBI, CBZ, CBR — both natively and under `-Dsanitize=address`. The CBZ run (Berserk v25, 212 pages) peaks at 789 MB under ASan, confirming the bytes-aware cache cap below is genuinely needed for comic-heavy use. The CBR run (From Hell, 583 pages, RAR-streamed) renders very few surfaces during the test window — the streaming-RAR cost noted in `fw-document-cbr.c` is real. (v0.15)
- [x] **`stress-zoom-storm`** — pins a single page in priority and runs 50 zoom cycles across realistic zoom levels (25%–400%, alternating direction). Each cycle bumps `render_gen`, exercises the v1.4 `prev_surface` stash and the v1.5 texture-before-surface unref ordering. Peak RSS during transition is allowed to be high (a typical run hits ~1.2 GB on the Effective Java sample); the leak signal is **post-settle current RSS** read from `/proc/self/status` (not `getrusage`'s high-water mark). Cap is `FW_STRESS_RSS_CAP_MB` (default 1024 MB). Verified clean natively and under `-Dsanitize=address`. The post-storm RSS drops from ~1218 MB peak to ~660 MB, confirming the prev_surface lifecycle correctly releases transient memory. (v0.15)
- [x] **`stress-multidoc`** — Sequential 50 open/close cycles across the 6-format corpus + parallel 10-instance phase + reverse-order dispose. Caught a real leak on first run: `g_thread_pool_free(pool, immediate=TRUE, ...)` was discarding queued `RenderJob` structs without invoking their workers' free path. Fixed by switching to `immediate=FALSE` so workers drain the queue via the cancel-bail path. ASan-clean after fix. Registered in `meson test`. (v0.25)
- [x] **`stress-corpus-soak`** — Full-corpus walk: opens each of the seven canonical samples (PDF×2, DjVu, EPUB, MOBI, CBZ, CBR), pushes priority on every fifth page through the cache up to 200 pages per doc, asserts no crashes / no open failures / peak RSS under 1.8 GB. Registered with `meson test`; clean under ASan+UBSan. The `--max-time` and per-file timing JSON are deferred — current shape is sufficient as a regression net for backend coverage. (v0.26)

### 12.3 Benchmarks (the "is it still fast" set)

- [x] **`bench-render`** — Cold + warm pass over an evenly-spaced span of pages on a single document (default 50, override `--pages` / `--stride` / `--zoom`). Reports n / mean / p50 / p95 / p99 / max in milliseconds plus total elapsed. The cache is intentionally bypassed — measures backend speed, not cache effectiveness. Built but not registered as a meson test (latency benchmark, not pass/fail). The JSON-output + commit-keyed regression-diff layer is deferred; manual runs are sufficient until we have a CI gate to feed the diffs into. (v0.26)
- [ ] **`bench-cache-hit-rate`** — drives the cache through a recorded scroll trace and reports hit/miss ratio per state (STATIC/CRUISING/SCRUBBING). Trace files live in `tests/traces/*.scroll` (binary, replayable). Useful for tuning the bytes-aware cache cap (Phase 11 Tier 1) without manual A/B testing.
- [x] **`bench-startup`** — Times each corpus sample's `fw_document_new_for_path` (open_ms) and the wait until `fw_cache_get_texture(0)` first returns non-NULL (first_paint_ms). Reports per-file timings as a table; built but not registered with `meson test`. Default corpus = the seven canonical samples; pass paths as argv to override. (v0.29)

### 12.4 Debugging setup

- [x] **Sanitizer build option** — `-Dsanitize=` array option in `meson_options.txt`, choices `address` / `undefined` / `leak` / `thread`. Forwarded to compile and link as `-fsanitize=` flags. ASan verified working on Brandon's Fedora; `-Dsanitize=undefined` and `leak`/`thread` require additional runtime libs (`libubsan`, `liblsan`, `libtsan`) not currently installed. `stress-scrub` runs clean under `-Dsanitize=address`. The `LSAN_SUPPRESSIONS` file (`tests/lsan.supp`) for known MuPDF/DjVu statics is still TODO — add it when leak detection is enabled. (v0.15 partial)
- [ ] **`tests/scripts/debug.sh`** — wraps `gdb -batch` with the canonical batch flags used in `CLAUDE.md`, plus pre-loaded breakpoints in `tests/scripts/framework.gdb` for `fz_catch` (the silent-warning trap), `cache_entry_free`, `submit_next_jobs`, and `render_worker`. One command for "open this file under gdb with all the right hooks."
- [ ] **`tests/scripts/coredump-triage.sh`** — given a coredump file or PID, runs `coredumpctl debug` non-interactively, captures `thread apply all bt`, MuPDF context state from each `RenderInstance`, and the cache state via a custom `gdb` pretty-printer for `FwCache`. Output goes to `~/.local/share/framework/triage/<timestamp>/`.
- [ ] **Trace-domain matrix** — `tests/scripts/trace-replay.sh <log>` parses `FW_DEBUG=1` output and produces a timeline (cache state transitions, render queue depth, eviction events) as a single SVG. Catches "why did it freeze" without manual log reading. Lightweight: just awk + svgwrite.
- [ ] **`framework --self-test`** — built only with `-Dstress=true`, runs a 5-second smoke test: opens a known sample, scrubs through it, validates a hash of the rendered first-page surface against a baseline. Used by the Flatpak CI pipeline (Phase 10) to catch toolchain regressions before shipping.

## Phase 13: Layout & Reflow Architectures
*Structural shifts for specific formats beyond the single-canvas raster paradigm. Focus on rich aesthetics and format-specific native behaviors.*

- [ ] **Fractal-Style EPUB Reflow** — Bypass MuPDF's fixed-layout engine for EPUBs (and reflowables). Parse the XHTML/DOM and map structural blocks into a `GListModel`, rendered via `GtkListView` using native GTK widgets (`GtkLabel` with Pango). This provides true reflow on window resize, native text selection, drops heavy Cairo surface caching, and achieves a gorgeous, modern aesthetic identical to Fractal's list architecture.
- [x] **Comic Facing Pages (Left/Right)** — `FwView::facing_pages` GSettings boolean (F10, primary menu under Comic Layout). `recompute_layout` builds a `pair_partner[]` array with **aspect-ratio-based spread detection** (v0.27.1): page 0 is the standalone cover; pages with `w/h > 1.0` are pre-rendered spreads and stand alone, orphaning their natural partner so alternation resumes after; everything else pairs (1+2, 3+4, ...). The snapshot path centers each pair as a unit with a fixed gutter; `view_pair_first` is consulted by `fw_view_get_current_page` so the header label tracks the lower-numbered page of the active pair. Mismatched page heights handled — pair height = max(h0, h1). Click-to-doc mapping (`fw_view_widget_to_doc`) walks both pages of the row before falling through. (v0.27, spread detection v0.27.1)
- [x] **Manga Mode (Right-to-Left)** — `FwView::manga_mode` GSettings boolean (F4, primary menu). When on, Left Arrow advances to next page and Right Arrow goes to previous (RTL reading order). Combined with facing-pages, also flips left/right within each pair so the lower-numbered page sits on the right. Pure scroll behavior unaffected when facing-pages is off — only directional page-nav keys swap. (v0.27)
- [x] **Webtoon Mode (Infinite Canvas)** — `FwView::webtoon_mode` GSettings boolean (F5, primary menu). Drops `PAGE_GAP` to zero in `recompute_layout` so vertically-laid-out long-strip comics stitch into a seamless single canvas. No-op when facing-pages is also active. (v0.27)

## Phase 14: Best-in-Class UX (The "Sumatra Clone" Polish)
*Features identified across reference repositories that elevate the viewer from 'functional' to 'exceptional'. Translated into native GTK/GNOME idioms.*

- [x] **Smart Text Selection (from SumatraPDF)** — Implemented via a `select_at(page, x, y, FwSelectGranularity)` interface method backed by MuPDF's `fz_snap_selection` against the v0.18 cached `fz_stext_page`. The view's existing `on_click_pressed` handler branches on `n_press`: 2 = word, 3 = line. Multi-press takes priority over link clicks (user intent is selection, not nav). DjVu and CBR backends return FALSE — drag selection still works on DjVu, CBR has no text. Sumatra's `FindClosestGlyph`/`SelectWordAt` pattern was the conceptual reference; the implementation uses the equivalent MuPDF builtin since fz_snap_selection does the same thing the canonical way. (v0.19)
- [x] **Seamless Auto-Reload (from SumatraPDF / Zathura)** — `GFileMonitor` attached to the open document path; reacts to `CHANGES_DONE_HINT` and `CREATED` events with a 200 ms debounce, saves state via `fw_state_save`, calls `fw_window_open_file` again with the same path, restores scroll via the existing deferred `restore_state_tick`. `AdwToastOverlay` was added to the window content tree to host an `AdwToast` ("Document updated") on each reload. `WATCH_HARD_LINKS` flag covers atomic-rename editors. Backend-agnostic — DjVu and CBR get auto-reload for free even though the design driver was the PDF/LaTeX flow. (v0.21)
- [x] **Margin Cropping (from Plato / Sioyek)** — Auto-detect via the v0.18 cached `fz_stext_page`: union of all char bboxes gives the content rect, ratio'd against the page size for fractional margins. Applied uniformly across every page (assumes uniform margins, which holds for technical PDFs; user can toggle off if a doc has variable margins). `recompute_layout` shrinks page sizes by the crop factors; the snapshot path draws the full texture offset+clipped so content fills the cropped rect. `(page, frac)` anchor preservation across the layout change. Toggle: `F6`, primary menu, GSettings-backed. PDF (and all MuPDF-routed formats including EPUB/MOBI) supported; DjVu/CBR return FALSE from `get_content_bbox` — toggle has no effect on those. (v0.25)
- [x] **Visual Ruler / Reading Mark (from Sioyek)** — Two semi-transparent black `gtk_snapshot_append_color` rects above and below a ~56-px clear band tracking mouse Y, gated by a `reading-ruler` GSettings boolean (default off). Toggleable via the **Reading Ruler** primary-menu entry or the **F8** keyboard shortcut. The shortcut is documented in the in-app Keyboard Shortcuts dialog and in `README.md`. Setting persists; menu checkmark and F8 stay in sync via `g_settings_create_action`. (v0.23)
- [x] **Interactive Loupe / Magnifying Glass (from YACReader)** — `gtk_snapshot_push_rounded_clip` (corner radius == half side = circle) + scale-around-cursor transform + re-append the cached page texture inside the clip + thin border. Toggleable via primary-menu "Magnifying Loupe" or **F7** shortcut, gated by the `loupe` GSettings boolean. 80 px radius, 2.5× zoom — both hardcoded for simplicity. Documented in README and the in-app Keyboard Shortcuts dialog. (v0.24)

### Explicitly NOT borrowing

- **Sumatra**: tile-as-default rendering (only as fallback per Tier 2); Win32-anything (`UpdateBitmapColors`, `CRITICAL_SECTION`, `HBITMAP`, GDI paint paths); `Func1<>` callback templates; `EngineEbook`/`EngineChm`/`EngineImages`/`EnginePs` (out of scope per spec §13).
- **Zathura**: girara UI; vim keybindings; per-page widget model (our single-canvas paint is a deliberate choice); `epub.css` user stylesheets.
- **Sioyek**: Qt5/QML stack; OpenGL textures (we use `GdkTexture` + `gtk_snapshot_append_texture`); PhD-research features (custom highlights, ruler mode, portal-style links).
- **Plato**: e-ink-specific UI; `framebuffer/` direct-to-fb rendering; single-threaded `Rc<Context>` model (loses our parallelism).
- **mupdf-gl**: single-threaded design; in-tree custom UI toolkit.

## Phase 15: The 1.0 Release (Concrete)
*Getting it out the door. Moved to the end after the 0.x sprint outgrew its original landing slot — Brandon explicitly does not want flathub submission until the rest of the roadmap is closed out.*

- [ ] **Application Assets** — Finalize SVG icon and `io.github.virinvictus.framework.desktop` file.
- [ ] **AppStream Metadata** — Write the XML for software centers.
- [ ] **Flatpak Manifest** — Build the GNOME 50 Flatpak. Test strict container compilation for MuPDF/DjVuLibre.
- [ ] **Permissions Audit** — Lock down the Flatpak sandbox. File access only via portal, no unnecessary network access.
- [ ] **Tag 1.0.0 & Release**

---

## Deferred (v2.0+ / Strictly Out of Scope for 1.0)
*Listed so the architecture doesn't preclude them, but forbidden from development until 1.0 ships.*

- [ ] Single page view mode
- [ ] Thumbnail sidebar (alternative to TOC)
- [ ] Annotations (highlight, underline — stored externally)
- [ ] Presentation mode (page-at-a-time, no chrome)
- [ ] Smooth pinch-to-zoom on touchscreens
- [ ] Configurable keybindings via GSettings
