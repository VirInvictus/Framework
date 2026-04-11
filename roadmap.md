# Framework — Roadmap

What's done, what's next, what's deferred. Updated as of v1.2.0.

---

## Done

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

---

## v1.2 — The Velocity Engine (Architecture Pivot)

*Replacing the brute-force static cache with intelligent resource pacing.*

- [x] **64MB MuPDF Clamp** — Hardcode `fz_new_context` limit to drop baseline RAM footprint.
- [ ] **Two-Tier Cache Split** — Separate parsed backend objects (low RAM) from rendered cairo surfaces (high RAM).
- [x] **Velocity Tracker** — Implement `dy/dt` calculation via `gtk_widget_add_tick_callback`.
- [x] **Dynamic Queue Management** — Implement Static, Cruising, and Scrubbing render states based on velocity thresholds.
- [x] **High-Velocity Abort** — Ensure `FwCache` drops all queued jobs instantly when the user scrubs, preventing CPU spin-up and DjVu mutex locks.
- [x] **Thread Drip-Feeding** — Force the `GThreadPool` worker to evaluate velocity after every single page render before picking up the next job.
- [x] **Surgical Mutexing** — Move Cairo surface copying outside the MuPDF global lock to prevent memory doubling during transit.
- [ ] **Make sure DJVU & PDF interactions are similar**
- [ ] **A button for downloading stream objects from PDF**

## v1.3 — Interaction

Core interaction features that SumatraPDF has and we don't yet.

- [x] **Ctrl+scroll wheel zoom** — zoom in/out with Ctrl held during scroll events
- [ ] **Search result highlighting** — paint semi-transparent yellow overlay on matching text regions across visible pages
- [ ] **Search next/prev** — F3 / Shift+F3 to cycle through matches, scroll to current match
- [ ] **Search match count** — "3 of 47 matches" label in the search bar
- [ ] **Invert colors** — bitwise NOT on RGB channels of rendered surfaces, toggle with Ctrl+I
- [ ] **Fit-page zoom** — scale so the entire page fits in the viewport (Ctrl+2)
- [ ] **Rotation** — Ctrl+Shift+Plus / Ctrl+Shift+Minus, 90-degree increments
- [ ] **Printing** — GtkPrintOperation, render pages to the print context's cairo surface (Ctrl+P)

## v1.4 — Text & Links

- [ ] **Text selection** — click-drag to select text using backend `get_text` with rectangle
- [ ] **Copy selected text** — Ctrl+C
- [ ] **Selection overlay** — blue semi-transparent rectangle over selected region
- [ ] **Text cursor** — cursor changes to I-beam over selectable text areas
- [ ] **Internal link navigation** — click a link to jump to the target page
- [ ] **External link handling** — open URLs in default browser via `g_app_info_launch_default_for_uri`
- [ ] **Link cursor** — pointer hand cursor over link areas
- [ ] **Navigation history** — Alt+Left / Alt+Right to go back/forward after link jumps

## v1.5 — Polish

- [ ] **Keyboard shortcuts dialog** — GtkShortcutsWindow showing all bindings (Ctrl+?)
- [ ] **Document properties dialog** — metadata display (title, author, page count, file size)
- [ ] **Scroll position in state** — restore sub-page scroll precision across zoom changes
- [ ] **Sidebar TOC highlight** — highlight the current section in the TOC as user scrolls
- [ ] **Sidebar click navigation** — clicking a TOC entry navigates to that page
- [ ] **Empty window state** — centered "Open a Document" button when no file is loaded
- [ ] **Drag-and-drop** — drop a file onto the window to open it
- [ ] **GtkTreeView deprecation** — migrate sidebar from GtkTreeView to GtkListView/GtkTreeListModel

---

## Deferred (v2.0+ / maybe never)

These are explicitly out of scope for the near term. Listed so the architecture
doesn't preclude them.

- [ ] Single page view mode
- [ ] Facing pages view mode
- [ ] Thumbnail sidebar (alternative to TOC)
- [ ] Annotations (highlight, underline — stored externally)
- [ ] Presentation mode (page-at-a-time, no chrome)
- [ ] Additional MuPDF formats (EPUB, XPS, CBZ)
- [ ] Smooth pinch-to-zoom on touchscreens
- [ ] Configurable keybindings via GSettings
- [ ] Flatpak packaging and Flathub submission
- [ ] LRU eviction when state.json exceeds 500 entries
