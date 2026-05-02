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

- [x] **GThreadPool sort-function priority** — `RenderJob` carries `last_view_time`; the pool runs `g_thread_pool_set_sort_function (render_job_compare)`; `submit_next_jobs` pushes all unrendered in-window pages at once and lets the pool reorder by recency. The asymmetric "near vs far" priority tiers and the `active_jobs < job_limit` throttle are gone. Pattern from zathura's `zathura/render.c:94`. (v0.14)

- [x] **Symmetric ±10-page parsed window with toggleable kinetic scrolling** — Priority window is now visible + 10 forward + 10 backward, interleaved, in all non-SCRUBBING states. A capture-phase `GtkEventControllerScroll` on `FwView` caps each scroll event at 90 px (wheel ticks scaled via `SCROLL_WHEEL_STEP = 60` first), applied directly to the vadjustment; this replaces GTK's default kinetic momentum scrolling. The cap is gated by a new GSettings key `kinetic-scrolling` (bool, default false) wired to a primary-menu toggle "Kinetic Scrolling" via `g_settings_create_action` — flipping the menu item switches behavior live. Default off = cache-friendly (the new project default); on = momentum flick for "gliding through research/school." (v0.14)

- [x] **Startup blur on saved-state open (regression)** — Real fix, not the predicted "obviated for free." Root cause was `update_cache_priority` in `fw-view.c` bailing on `gtk_widget_get_height <= 0` during state-restore (the adjustment value-changed fires before allocation settles). Fix adds a fallback path: when widget height is unallocated, derive the visible page from `page_y_offsets[]` and push that single page as priority. Combined with the sort-function dispatch above, the saved-page job lands ahead of the initial pages 0–13 queue and renders before the window first paints. (v0.14)

- [x] **Bytes-aware cache cap** — `CACHE_WINDOW = 30` replaced by `total_cached_bytes` tracking + `byte_cap` (512 MB default, `FW_CACHE_BYTES_CAP_MB` env override). Accounting is live at every surface store/replace/evict path under the existing mutex; `cache_entry_free` is documented as not auto-decrementing (callers handle it). Eviction fires only when over cap and prefers outside-priority pages (visible + ±10 are never evicted). The new policy keeps outside-priority surfaces in cache until cap is hit, so scroll-back is instant when there's headroom — the v1.3.3 "drop everything outside window" behavior is gone. Verified across all six backends (PDF/DjVu/EPUB/MOBI/CBZ/CBR), and the `stress-scrub` test pins a tight 128 MB cap to exercise the eviction path during Phase 4 (slow walk). Under ASan: clean. Pattern from sumatra's `src/RenderCache.cpp` `FreeIfFull` translated to GLib idioms. (v0.16)

### Tier 2 — Medium-impact / post-1.0

- [x] **`try_closest_rendered_page` zoom transition** — `CacheEntry` now retains up to `MAX_PREV_ZOOM_SLOTS = 3` previous-zoom snapshots in a `ZoomSlot[]` array (each carrying `zoom`/`rotation`/`scale_factor`/`size_bytes`). `fw_cache_start` demotes the current surface into `prev_slots[0]`, shifts older slots right, evicts the oldest on overflow. `fw_cache_get_texture` returns the slot with minimal `|zoom − target|` (matching rotation+scale) when the current-gen surface isn't ready — GTK's `gtk_snapshot_append_texture` already auto-scales, so the view code didn't need a transform change. Bytes accounted in `total_cached_bytes` so the existing 512 MB byte_cap still bounds RAM. ASan-clean; `stress-zoom-storm`'s settled-RSS cap was raised 1024 → 1280 MB to accommodate the legitimate extra cache. (v0.28)

- [~] **High-zoom render-bytes cap** (shipped pragmatic alternative to tile slicing). Per-render allocation capped at 64 MB; over-cap renders scale `render_zoom` down so the surface fits, and GTK upscales the resulting texture to fill the requested rect on paint. `entry->zoom` is set to the effective post-cap value so `try_closest_rendered_page` (v0.28) retains capped renders correctly. Same memory bound as the original tile-slice spec in ~10 LOC vs. several hundred for a real slice path. True N×M slicing remains a follow-up if a poster-PDF need ever surfaces. (v0.35)

- [x] **TTL + LRU hybrid cache eviction** — `last_access_us` field added to `CacheEntry`; bumped on every `fw_cache_get_page` / `fw_cache_get_texture` hit and on worker-store success. Bytes-cap eviction loop now sorts outside-priority candidates oldest-first via a fileop-scope `LruVictim` GArray. The 1 Hz `g_timeout_add` GC layer is intentionally skipped — eviction-on-overflow already drops cold pages, and a separate idle GC would race the workers without measurable benefit. (v0.26)

- [x] **Hue-preserving recolor (smarter "Invert Colors")** — Replaces the bitwise-NOT 4×4 with a luminance-aware affine: compute BT.601 luma `Y` and add `(1−2Y)` to every channel, preserving each pixel's chromatic offset (R−Y, G−Y, B−Y). Red stays red, blue stays blue, but the lightness axis flips. Single `gtk_snapshot_push_color_matrix`, GPU-side, GSK clamps gamut for free. Configurable `recolor-light` / `recolor-dark` GSettings keys (the full zathura-style customization) remain a follow-up — current implementation hardcodes the standard white↔black mapping. (v0.22)

- [x] **Per-instance MuPDF store size scaling** — `pdf_pick_store_size(path)` helper picks 16/32/64/128 MB based on `g_stat` of the file (under 5 / 20 / 100 MB / above). Both `fz_new_context` call sites — main + per-instance render context — use the scaled value. Worst-case is now 1 GB across all 8 contexts on heavy textbooks (down from "uniform 32 MB regardless of doc size", which both wasted RAM on novels and pinched on PDF/A-3 textbooks with embedded JPEG2000 + TrueType). bench-render p50 cold/warm confirms ~3× store-hit speedup. (v0.26)

### Tier 3 — Investigation / measurement before commit

- [~] **Reconsider 8× `fz_open_document` model** — *Validated by measurement (v0.36); current model retained.* The pre-1.0 stress harness exercises the worst real-world memory cases this Tier 3 entry was hedging against:
  - `stress-multidoc` runs 10 `FwDocument` + `FwCache` instances *simultaneously* (50 sequential open/close cycles plus 10 held in parallel, all backends) and lands under 2 GB peak RSS even with ASan instrumentation overhead.
  - `stress-corpus-soak` walks every fifth page of all 7 corpus samples (PDF×2, DjVu, EPUB, MOBI, CBZ, CBR) and lands at 1.5 GB peak RSS.
  - `bench-render` reports a clean 3× warm/cold render-time speedup on Effective Java, confirming the per-instance MuPDF stores are doing their job at the v0.26-scaled sizes (16/32/64/128 MB).

  Reasoning: the 8-instance model's worst case is one open doc × 8 contexts × max 128 MB stores = 1 GB of stores, plus thumbs (~120 MB) and the 512 MB cache byte cap = ~1.6 GB ceiling. The original concern was 4 GB low-RAM laptops where that ceiling is 40% of RAM. On the development target (Brandon's 30 GB Fedora) and on any contemporary laptop with 16+ GB, the headroom is comfortable. The measured corpus never exceeds the cap.

  Decision: keep the current independent-document model. The clone-context refactor is a real architectural change with non-trivial threading risk (locks-callback contract, per-thread context lifetime, store-eviction semantics across cloned contexts) for a benefit that hasn't materialized. **Revisit if** an actually-memory-constrained target (Plato-style e-reader port, Flatpak sandbox memory limit, embedded distro) becomes a real deployment target — the roadmap entry stays as a tracked option.

- [~] **Per-thread-shared-context hybrid** — *Validated by measurement (v0.36); current model retained.* Sioyek's middle-ground (one parent context, `fz_clone_context` per render thread, per-`(thread, path)` document) has the same memory-savings rationale as the previous item; the same measurement reasoning applies. Adopting it would also lose the "per-instance lock means workers never wait on each other" property that makes the current model dead-simple to reason about. Tracked as a fallback if memory constraints ever push us off the 8-independent-document model.

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
- [x] **`bench-cache-hit-rate`** — Drives the cache through three synthetic scroll patterns (STATIC walk, CRUISING walk, SCRUBBING jumps) and reports hit ratio per pattern. Recorded trace files were dropped in favor of inline patterns — no recorder infrastructure, fully reproducible. Built but not registered with `meson test`. (v0.30)
- [x] **`bench-startup`** — Times each corpus sample's `fw_document_new_for_path` (open_ms) and the wait until `fw_cache_get_texture(0)` first returns non-NULL (first_paint_ms). Reports per-file timings as a table; built but not registered with `meson test`. Default corpus = the seven canonical samples; pass paths as argv to override. (v0.29)

### 12.4 Debugging setup

- [x] **Sanitizer build option** — `-Dsanitize=` array option in `meson_options.txt`, choices `address` / `undefined` / `leak` / `thread`. Forwarded to compile and link as `-fsanitize=` flags. ASan verified working on Brandon's Fedora; `-Dsanitize=undefined` and `leak`/`thread` require additional runtime libs (`libubsan`, `liblsan`, `libtsan`) not currently installed. `stress-scrub` runs clean under `-Dsanitize=address`. The `LSAN_SUPPRESSIONS` file (`tests/lsan.supp`) for known MuPDF/DjVu statics is still TODO — add it when leak detection is enabled. (v0.15 partial)
- [x] **`tests/scripts/debug.sh`** — gdb-batch wrapper with breakpoints from `framework.gdb` (`fz_throw`, `cache_entry_free`, `submit_next_jobs`) loaded; runs `./builddir/src/framework "$@"` with `GSETTINGS_SCHEMA_DIR` auto-set, prints multi-thread bt + registers on crash. Used for one-shot crash investigation. (v0.31)
- [x] **`tests/scripts/coredump-triage.sh`** — Non-interactive coredump capture. Resolves the dump via `coredumpctl dump --output` (rather than `coredumpctl debug` which interleaves its own info with gdb's output and breaks capture), then runs `gdb -batch` directly against the binary+core. Writes to `~/.local/share/framework/triage/<UTC-timestamp>/`: coredumpctl info, command line, full gdb session (multi-thread bt + registers + maps), and a per-thread bt slice. The custom `FwCache` pretty-printer is left as a placeholder for follow-up. (v0.32)
- [x] **Trace-domain matrix** — `tests/scripts/trace-replay.sh` parses `FW_DEBUG=1` output and emits an SVG timeline with five tracks: state bands (STATIC/CRUISING/SCRUBBING), zoom changes, worker start/done ticks, byte-cap eviction triangles. Pure bash + awk (no Python), reads from a file argument or stdin. (v0.33)
- [x] **`framework --self-test`** — Built only with `-Dstress=true` (new `FW_STRESS_BUILD` config define). Opens a known-good document (argv[2] override or hardcoded corpus default), drives the cache through the open path + a 12-step stride scrub, asserts page 0 paints within 5 seconds. Hash-baseline check from the original spec dropped — too brittle across MuPDF versions / platform fonts to be useful CI signal; the smoke gate checks the property that actually breaks on toolchain regressions (binary opens, page 0 paints, no crashes). Non-stress builds reject `--self-test` with exit 2 and a clear message. (v0.34)

## Phase 13: Layout & Reflow Architectures
*Structural shifts for specific formats beyond the single-canvas raster paradigm. Focus on rich aesthetics and format-specific native behaviors.*

- [~] **Fractal-Style Reflow Rewrite** — Native-GTK reflow for EPUB / MOBI / AZW3 / FB2 / TXT, bypassing MuPDF's fixed-layout engine for these formats. Design locked in `docs/fractal-rewrite.md` (v0.36 commit) — covers format scope (LIT/CHM/DOCX/RTF explicitly skipped), `FwReflowDocument` interface, `FwReflowView` GtkListView pattern, format-specific implementation notes for each backend, and a 6-phase shipping plan.
  - [x] **Phase 1 — `FwReflowView` + TXT backend** (v0.40.0). `FwReflowDocument` interface, `FwBlock` GObject, `FwReflowTocItem` GObject, `FwReflowDocumentTxt` backend with UTF-8 / UTF-16 BOM detection + Latin-1 fallback + Pango-escape, `FwReflowView` widget hosting `GtkListView` + `GtkSingleSelection` + `GtkSignalListItemFactory` driven by per-kind CSS classes, 720 px reading-column cap, extension-based dispatch in `fw-window.c` between the fixed-layout and reflow pipelines, `*.txt` + `text/plain` added to the file dialog. ASan + UBSan clean. Caught one transfer-full ownership bug on `gtk_list_view_new` during smoke test (see v0.40.0 patchnote).
  - [x] **Phase 2 — `FwReflowDocumentFb2`** (v0.41.0). `GMarkupParser`-based walker producing CHAPTER markers + nested HEADING (level = section depth) + PARAGRAPH (with Pango markup for emphasis/strong/code/sub/sup/strikethrough) + BLOCKQUOTE (`<cite>`/`<epigraph>`) + HR (`<empty-line/>`) + IMAGE (resolved against `<binary>` base64 → `GdkTexture`). TOC from `<section>` titles with anchor ids. Metadata from `<title-info>` (title / author / lang / annotation). Three `href` prefixes accepted (`l:href`, `xlink:href`, `href`). Non-UTF-8 XML encodings converted via `g_convert` before parsing. ASan + UBSan clean. Bare `.fb2` only — `.fb2.zip` follow-up; image rendering still falls through to the text label fallback in `FwReflowView` (binaries parsed and held for the upgrade).
  - [x] **Phase 3 — `FwReflowDocumentEpub`** (v0.42.0). The marquee delivery. libarchive ZIP read (one streaming pass into a path → GBytes hash); `META-INF/container.xml` → OPF; OPF manifest + spine + `dc:` metadata; per-chapter XHTML through `GMarkupParser` into HEADING (`<h1..h6>`) / PARAGRAPH (`<p>`) / BLOCKQUOTE (`<blockquote>`) / CODE (`<pre>`) / IMAGE (`<img src>`) / HR (`<hr>`) / `<li>` → `"•  "`-prefixed paragraph; inline tags → Pango markup; whitespace collapsed; chapter CHAPTER markers emitted lazily on first content block, anchored to the resolved chapter path so NCX `<content src=...>` lookups resolve. NCX → TOC. Manifest images decoded to `GdkTexture` and keyed by both resolved zip path and manifest id. Tested clean on five real Calibre-generated EPUBs (Denning, Cahill, Hill, Brown, Stirner); ASan + UBSan clean; all 5 stress tests still pass. Strict-parser and NCX-only TOC for now — libxml2 tolerant HTML and EPUB 3 nav.xhtml are follow-ups if real-world breakage forces them.
  - [ ] **Phase 4 — `FwReflowDocumentMobi`**. PalmDOC LZ77 decompressor + KF7 path. Hardest of the in-scope formats; ship after EPUB.
  - [ ] **Phase 5 — `FwReflowDocumentAzw3`**. KF8 on top of MOBI's PalmDOC.
  - [ ] **Phase 6 — Polish**. Font-size adjustment, "X% read" indicator, search-hit highlight in the GtkListView, chapter sidebar, fall-through-to-MuPDF toggle for difficult docs.
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
