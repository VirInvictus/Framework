# Framework — Roadmap

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

## Phase 3: Spatial Navigation
*How the user moves around the document. It must feel physical and precise.*

- [x] **Ctrl+Scroll Wheel Zoom** — Anchor zoom to the pointer coordinates, not the viewport center.
- [ ] **Fit-Page & Fit-Width** — (Ctrl+1, Ctrl+2). Re-calculate bounding boxes on widget resize allocations.
- [ ] **Rotation** — (Ctrl+Shift+Plus/Minus). 90-degree increments, invalidating and re-rendering the Cairo cache.
- [ ] **Scroll Position Precision** — Restore sub-page scroll coordinates accurately across zoom level changes.

## Phase 4: Text & Geometry
*Extracting data from the render without locking the UI thread.*

- [ ] **Text Selection** — Click-drag to select text using backend `get_text` with rectangle calculations.
- [ ] **Selection Overlay** — Paint a semi-transparent blue GTK overlay over the selected region (avoid re-rendering the base PDF).
- [ ] **Copy to Clipboard** — Pipe selected text directly to `GdkClipboard` (Ctrl+C).
- [ ] **Dynamic Cursors** — I-beam over selectable text, pointer hand over link areas.

## Phase 5: Search & Illumination
*Finding data fast.*

- [ ] **Async Search** — Pass search queries to a background worker. 
- [ ] **Search Result Highlighting** — Paint semi-transparent yellow overlays on matching text regions.
- [ ] **Search Navigation** — F3 / Shift+F3 to cycle through matches, automatically scrolling to the active match.
- [ ] **Match Count** — "3 of 47 matches" label in the search bar.

## Phase 6: Document Topology
*Moving through the structure of the file.*

- [ ] **Sidebar TOC Highlight** — Track the current page and highlight the active section in the sidebar during scrolling.
- [ ] **Sidebar Click Navigation** — Clicking a TOC entry jumps immediately to the target page.
- [ ] **Internal Link Jumps** — Clicking a linked footnote or index item navigates to the target page.
- [ ] **Navigation Stack (History)** — Alt+Left / Alt+Right to go back/forward after jumping via links or TOC.

## Phase 7: Format Expansion
*Leveraging MuPDF to open everything in the anti-library.*

- [ ] **Comic Books (CBZ/CBR)** — Wire up the archive backend for graphic novels.
- [ ] **EPUB / XPS Support** — Hook up the remaining MuPDF format parsers.
- [ ] **External Links** — Open web URLs in default browser via `g_app_info_launch_default_for_uri`.

## Phase 8: Desktop Symbiosis
*Making it a native, well-behaved GTK citizen.*

- [ ] **GtkListView Migration** — Deprecate `GtkTreeView` for the sidebar. Move to the faster `GtkListView`/`GtkTreeListModel`.
- [ ] **Empty Window State** — Centered "Open a Document" button when no file is loaded.
- [ ] **Drag-and-Drop** — Drop a file onto the window to open it.
- [ ] **Printing** — Implement `GtkPrintOperation`, rendering pages to the print context's cairo surface (Ctrl+P).

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
