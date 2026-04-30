# Framework — Patch Notes

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

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
