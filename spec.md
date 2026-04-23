# Framework — Application Specification

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

**Version:** 1.0  
**Target:** GNOME 50+, GTK4/libadwaita  
**Language:** C (C17)  
**Build System:** Meson  
**License:** GNU GPL v3.0 or later (to align with the GNOME ecosystem)

---

## Influence
Framework is heavily influenced by **SumatraPDF**'s philosophy: extreme performance, minimal UI, and a strict focus on being a viewer rather than an editor. It aims to be the "SumatraPDF for GNOME".

## 1. Mission Statement

Framework is a fast, native GNOME document viewer built on MuPDF. It renders PDF and DjVu files with aggressive pre-caching, a clean libadwaita UI, and zero bloat. It is a viewer — not an editor, not a library manager, not a file organizer. It opens documents, displays them beautifully, and stays out of the way.

Design philosophy: **accessible to a grandma, useful to a power user.** Every action has a visible UI control. Every UI control has a keyboard shortcut. No vim bindings, no modal interfaces, no hidden commands. SumatraPDF is the reference implementation; GNOME HIG is the law.

---

## 2. Architecture

### 2.1 Rendering Abstraction

Framework uses a backend abstraction layer to support multiple document formats through different rendering libraries.

```text
┌─────────────────────────────────────┐
│          FrameworkDocument           │
│  (abstract interface / vtable)      │
├─────────────────────────────────────┤
│  open(path) → bool                  │
│  page_count() → int                 │
│  page_size(n) → (w, h)             │
│  render_page(n, zoom, rotation)     │
│       → cairo_surface_t* │
│  get_toc() → FrameworkTocNode* │
│  search(text, page) → GArray* │
│  get_text(page, rect) → char* │
│  get_links(page) → GArray* │
│  close()                            │
└──────────┬──────────┬───────────────┘
           │          │
    ┌──────┴──┐  ┌────┴─────┐
    │  MuPDF  │  │ DjVuLibre│
    │ Backend │  │ Backend  │
    └─────────┘  └──────────┘
```

**MuPDF backend:** Links against `libmupdf`. Handles PDF, rendering to cairo surfaces via MuPDF's pixmap API. MuPDF is compiled/linked statically or as a shared library — Flatpak bundling is the primary distribution concern.

**DjVuLibre backend:** Links against `libdjvu` (ddjvuapi). Handles DjVu files. Rendering via DjVuLibre's own page rendering to pixel buffers, converted to cairo surfaces.

**Backend selection:** Determined at file open time by inspecting the file extension and/or magic bytes. `.pdf` → MuPDF. `.djvu` / `.djv` → DjVuLibre.

### 2.2 The Velocity-Driven Cache Engine

The pre-cache engine is the core performance differentiator. To balance rapid scrolling against memory boundaries and CPU thermal constraints, Framework uses a **Two-Tier, Velocity-Driven Architecture**, completely discarding static page counts.

#### Tier 1: The Parsed Window
A wide window (e.g., 30-50 pages) where the document structure is loaded into memory (MuPDF `fz_page` or DjVu equivalents), but **no pixels are rendered**. This eliminates disk I/O when navigating, at a negligible RAM cost.

#### Tier 2: The Pixel Window (Surface Cache)
A dynamic, strictly managed hash table of `cairo_surface_t` pixel buffers. Expansion and eviction are dictated by the user's kinetic scroll velocity (`dy/dt`), calculated via `gtk_widget_add_tick_callback` on the scrollable view.

**Velocity States & Strategies:**

1.  **Static (Velocity = 0):** The user is reading.
    * Action: Render visible pages, 2 pages ahead, 1 page behind.
    * Pacing: Yield the thread pool. Let the CPU drop to idle. Do not pre-render deeper.
2.  **Cruising (Moderate Velocity):** The user is reading at pace or scanning.
    * Action: Expand the forward cache to 5-8 pages. Drop the backward cache entirely.
    * Pacing: Drip-feed background renders *one at a time*. The worker must check velocity between each render to prevent CPU spikes.
3.  **Scrubbing (High Velocity):** The user has grabbed the scrollbar or flicked the wheel hard.
    * Action: **ABORT.** Clear the render queue. Do not attempt to render intermediate pages flying through the viewport. Paint grey placeholders.
    * DjVu Constraint: This is strictly enforced for DjVu to prevent the backend mutex from locking the application while trying to decode skipped pages.
4.  **View Changes:** On zoom or rotation, invalidate the Pixel Window. Stale surfaces are aggressively dropped if outside the immediate viewport to prevent 2x memory spikes.

```text
┌──────────────────────────────────────┐
│           FrameworkCache             │
├──────────────────────────────────────┤
│  surfaces: GHashTable<int, Surface>  │
│  parsed: GHashTable<int, BackendObj> │
│  thread_pool: GThreadPool            │
│  velocity: double (dy/dt)            │
├──────────────────────────────────────┤
│  update_velocity(dy_dt)              │
│  evaluate_queue_strategy()           │
│  render_next_job_and_yield()         │
│  abort_queue()                       │
└──────────────────────────────────────┘
```

**Thread safety:** MuPDF is thread-safe per-context (one `fz_context` per thread, cloned from parent). DjVuLibre requires serialized access — use a mutex or single worker thread for DjVu rendering.

### 2.3 Widget Tree

```text
AdwApplicationWindow
├── AdwHeaderBar
│   ├── [left]  AdwSplitButton (sidebar toggle)
│   ├── [left]  GtkButton (zoom out)
│   ├── [left]  GtkEntry (zoom percentage, editable)
│   ├── [left]  GtkButton (zoom in)
│   ├── [title] GtkLabel (filename)
│   ├── [right] GtkEntry (page number / total, editable)
│   ├── [right] GtkButton (previous page)
│   ├── [right] GtkButton (next page)
│   ├── [right] GtkToggleButton (search)
│   └── [right] GtkMenuButton (primary menu)
├── AdwOverlaySplitView
│   ├── [sidebar] GtkScrolledWindow
│   │   └── GtkTreeView / GtkListView (TOC)
│   └── [content] GtkOverlay
│       ├── GtkScrolledWindow
│       │   └── FrameworkView (custom GtkWidget — renders pages)
│       └── [overlay] GtkSearchBar
│           └── GtkSearchEntry + match count + prev/next buttons
└── [bottom] GtkActionBar (optional: status info, if needed)
```

### 2.4 Application Object

```text
FrameworkApplication : AdwApplication
├── Handles file open via command line args and GtkFileDialog
├── Single-instance (activate brings existing window forward)
├── No tabs, no multi-document — one document per window
│   Multiple files = multiple windows (via g_application_open)
├── Stores per-file state in XDG_DATA_HOME/framework/state.json:
│   { "/path/to/file.pdf": { "page": 42, "zoom": 1.5, "scroll_y": 0.73 } }
└── GSettings schema for preferences:
    - default-zoom-mode (fit-width / fit-page / percentage)
    - continuous-scroll (bool, default true)
    - default-view-mode (single / facing)
    - invert-colors (bool)
```

---

## 3. UI Specification

### 3.1 Header Bar

Standard AdwHeaderBar. No custom titlebar chrome. Follows GNOME HIG spacing and sizing.

**Left cluster — Sidebar + Zoom:**

| Control | Type | Behavior |
|---------|------|----------|
| Sidebar toggle | AdwSplitButton | Main click toggles TOC sidebar. Dropdown could offer Thumbnails (v1.1 candidate). Icon: `view-sidebar-symbolic` |
| Zoom out | GtkButton | Decrease zoom by 10% (or step through preset levels). Icon: `zoom-out-symbolic` |
| Zoom level | GtkEntry | Shows current zoom as "125%". User can type a number and press Enter. Validates input (clamp 10%-1000%). Width: ~5em |
| Zoom in | GtkButton | Increase zoom by 10%. Icon: `zoom-in-symbolic` |

**Center — Title:**

| Control | Type | Behavior |
|---------|------|----------|
| Document title | GtkLabel | Shows filename (not full path). Ellipsize end. Tooltip shows full path |

**Right cluster — Navigation + Actions:**

| Control | Type | Behavior |
|---------|------|----------|
| Page entry | GtkEntry | Shows "42 / 350". User can type a page number and press Enter to jump. Width: ~7em |
| Previous page | GtkButton | Go to previous page. Icon: `go-up-symbolic` |
| Next page | GtkButton | Go to next page. Icon: `go-down-symbolic` |
| Search toggle | GtkToggleButton | Shows/hides search bar. Icon: `edit-find-symbolic` |
| Primary menu | GtkMenuButton | App menu (see 3.4). Icon: `open-menu-symbolic` |

### 3.2 TOC Sidebar

Displayed via `AdwOverlaySplitView` so it overlays the content on narrow windows and sits beside it on wide windows. Populated from the document's outline/TOC structure.

- Tree view with expandable nodes
- Click a node → navigate to that page/destination
- Highlight current position in TOC as user scrolls
- If document has no TOC → sidebar shows a "No table of contents" placeholder
- Sidebar width: ~280px default, resizable is a nice-to-have

### 3.3 Main View Area (FrameworkView)

Custom `GtkWidget` subclass responsible for laying out and painting rendered pages.

**View modes:**

| Mode | Behavior |
|------|----------|
| Continuous scroll (default) | All pages stacked vertically with gaps, free scroll. This is the primary mode |
| Single page | One page at a time, page up/down to navigate |
| Facing pages | Two pages side-by-side, first page alone (like a book). Continuous scroll variant preferred |

**Zoom modes:**

| Mode | Behavior |
|------|----------|
| Fit width | Page width matches viewport width. Default |
| Fit page | Entire page visible in viewport |
| Custom percentage | 10% — 1000%, typed into zoom entry |

**Rendering pipeline:**

1. `FrameworkView` determines which pages are visible in the current scroll position
2. Requests rendered surfaces from `FrameworkCache`
3. If cache hit → paint immediately via `gtk_snapshot_append_texture`
4. If cache miss → paint placeholder (light gray rect with page number), queue priority render
5. Search highlights painted as semi-transparent overlay rectangles on top of page surfaces
6. Link regions stored for cursor change and click handling

**Scroll behavior:**

- Smooth scrolling via `GtkScrolledWindow` kinetic scroll
- Scroll position saved/restored per document
- Page gap: 8px (scaled) between pages in continuous mode

**Selection and copy:**

- Click-drag to select text (using backend's `get_text` with rectangle)
- Ctrl+C to copy selected text
- Selection rendered as blue semi-transparent overlay
- Cursor changes to text cursor over selectable text areas

**Link handling:**

- Internal links (to other pages): navigate on click
- External links (URLs): open in default browser via `g_app_info_launch_default_for_uri`
- Cursor changes to pointer hand over link areas

### 3.4 Primary Menu

```text
├── Open...                    (Ctrl+O)
├── ─────────────
├── Zoom
│   ├── Fit Width              (Ctrl+1)
│   ├── Fit Page               (Ctrl+2)
│   ├── Actual Size (100%)     (Ctrl+0)
│   └── ─────────────
│       Custom (shows current %)
├── View Mode
│   ├── Continuous             (radio)
│   ├── Single Page            (radio)
│   └── Facing Pages           (radio)
├── Rotate
│   ├── Rotate Clockwise       (Ctrl+Shift+Plus)
│   └── Rotate Counter-CW     (Ctrl+Shift+Minus)
├── ─────────────
├── Invert Colors              (Ctrl+I)
├── ─────────────
├── Print...                   (Ctrl+P)
├── Document Properties        (shows metadata dialog)
├── ─────────────
├── Keyboard Shortcuts         (Ctrl+?)
├── About Framework
```

### 3.5 Search Bar

Appears at top or bottom of the content area (GNOME convention: top, via `GtkSearchBar`).

| Control | Behavior |
|---------|----------|
| Search entry | Type to search. Search begins on Enter or after a debounce delay (~300ms) |
| Match count | Label showing "3 of 47 matches" |
| Previous match | Button, Enter+Shift or Shift+F3 |
| Next match | Button, Enter or F3 |
| Close | Escape or toggle button |

**Search behavior:**

- Search all pages (not just visible)
- Highlight all matches on all rendered pages with a distinct color (e.g., semi-transparent yellow)
- Current match highlighted with a different color (e.g., semi-transparent orange)
- Scroll to current match
- Wrap around at document end/beginning

### 3.6 Fullscreen Mode

- `F11` toggles fullscreen
- Header bar auto-hides, revealed on mouse movement to top edge (standard GNOME fullscreen behavior)
- Search bar remains accessible via `Ctrl+F`
- Escape exits fullscreen

### 3.7 No Welcome Screen

If launched without a file argument, show an empty window with a centered "Open a Document" button or just the header bar with the Open action available. No recent files grid, no library view, no tips. Minimal.

---

## 4. Keyboard Shortcuts

Sumatra defaults adapted to GNOME conventions. All shortcuts visible in the Keyboard Shortcuts dialog (`Ctrl+?`).

### Navigation

| Action | Shortcut(s) |
|--------|-------------|
| Next page | Page Down, Down (single page mode) |
| Previous page | Page Up, Up (single page mode) |
| First page | Home, Ctrl+Home |
| Last page | End, Ctrl+End |
| Go to page | Ctrl+G (opens page entry focused) |
| Scroll down | Down, j (continuous mode) |
| Scroll up | Up, k (continuous mode) |
| Back (history) | Alt+Left |
| Forward (history) | Alt+Right |

### Zoom

| Action | Shortcut(s) |
|--------|-------------|
| Zoom in | Ctrl+Plus, Ctrl+= , Ctrl+Scroll Up |
| Zoom out | Ctrl+Minus, Ctrl+Scroll Down |
| Fit width | Ctrl+1 |
| Fit page | Ctrl+2 |
| Actual size (100%) | Ctrl+0 |

### Search

| Action | Shortcut(s) |
|--------|-------------|
| Find | Ctrl+F |
| Find next | F3, Enter (in search bar) |
| Find previous | Shift+F3, Shift+Enter (in search bar) |
| Close search | Escape |

### View

| Action | Shortcut(s) |
|--------|-------------|
| Toggle sidebar | F9 |
| Fullscreen | F11 |
| Rotate clockwise | Ctrl+Shift+Plus |
| Rotate counter-clockwise | Ctrl+Shift+Minus |
| Invert colors | Ctrl+I |

### General

| Action | Shortcut(s) |
|--------|-------------|
| Open file | Ctrl+O |
| Print | Ctrl+P |
| Copy (selected text) | Ctrl+C |
| Select all (current page text) | Ctrl+A |
| Quit / Close window | Ctrl+Q, Ctrl+W |
| Keyboard shortcuts | Ctrl+? |

---

## 5. File Format Handling

### 5.1 Supported Formats

| Format | Backend | Extensions | MIME Types |
|--------|---------|------------|------------|
| PDF | MuPDF (libmupdf) | `.pdf` | `application/pdf` |
| DjVu | DjVuLibre (libdjvu) | `.djvu`, `.djv` | `image/vnd.djvu`, `image/x-djvu` |

### 5.2 Desktop Integration

Framework registers as a handler for the above MIME types via its `.desktop` file. It should declare itself capable of opening these types but should not aggressively claim default handler status over established apps on install.

### 5.3 File Open

- Command line: `framework [FILE...]` — each file opens in its own window
- GtkFileDialog with filter for supported formats
- Drag-and-drop onto window opens file (replaces current document, or new window — TBD, but single-document-per-window suggests new window)

---

## 6. State Persistence

### 6.1 Per-Document State

Stored in `$XDG_DATA_HOME/framework/state.json` (typically `~/.local/share/framework/state.json`).

```json
{
  "/home/brandon/documents/sicp.pdf": {
    "page": 142,
    "scroll_position": 0.73,
    "zoom_mode": "fit-width",
    "zoom_level": 1.0,
    "view_mode": "continuous",
    "rotation": 0,
    "last_opened": "2026-04-06T12:00:00Z"
  }
}
```

- Saved on document close or window close
- Restored on re-open of the same file (matched by absolute path)
- Prune entries older than 90 days on startup to prevent unbounded growth
- Maximum 500 entries (LRU eviction)

### 6.2 Application Preferences

Stored via GSettings. Schema: `com.github.vrnvctss.framework` (adjust namespace as appropriate).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `default-zoom-mode` | enum | `fit-width` | fit-width, fit-page, custom |
| `default-zoom-level` | double | 1.0 | Used when zoom-mode is custom |
| `continuous-scroll` | bool | true | Default view mode |
| `default-view-mode` | enum | `single` | single, facing |
| `invert-colors` | bool | false | Color inversion |
| `window-width` | int | 900 | Last window width |
| `window-height` | int | 700 | Last window height |
| `window-maximized` | bool | false | Last window state |
| `sidebar-visible` | bool | false | Last sidebar state |
| `sidebar-width` | int | 280 | Last sidebar width |

---

## 7. Dependencies

### Build Dependencies

| Dependency | Minimum Version | Purpose |
|------------|----------------|---------|
| gtk4 | 4.16+ | UI toolkit (GNOME 48 ships 4.16; GNOME 50 will ship 4.18+) |
| libadwaita | 1.7+ | GNOME design patterns |
| mupdf | 1.24+ | PDF rendering |
| djvulibre | 3.5.28+ | DjVu rendering |
| cairo | 1.18+ | Surface management, compositing |
| glib | 2.82+ | Data structures, threading, I/O |
| json-glib | 1.10+ | State persistence |
| meson | 1.4+ | Build system |

### Runtime Dependencies

All of the above as shared libraries, plus standard GNOME runtime (for Flatpak, this is the `org.gnome.Platform` runtime).

---

## 8. Build Configuration

### 8.1 Meson Project Structure

```text
framework/
├── meson.build                 # top-level
├── meson_options.txt
├── src/
│   ├── meson.build
│   ├── main.c                  # entry point, AdwApplication setup
│   ├── fw-application.c/h      # FrameworkApplication
│   ├── fw-window.c/h           # FrameworkWindow (AdwApplicationWindow)
│   ├── fw-view.c/h             # FrameworkView (custom page render widget)
│   ├── fw-cache.c/h            # Pre-cache engine
│   ├── fw-document.c/h         # Abstract document interface
│   ├── fw-document-pdf.c/h     # MuPDF backend
│   ├── fw-document-djvu.c/h    # DjVuLibre backend
│   ├── fw-sidebar.c/h          # TOC sidebar
│   ├── fw-search.c/h           # Search controller
│   └── fw-state.c/h            # Per-document state persistence
├── data/
│   ├── meson.build
│   ├── com.github.vrnvctss.framework.desktop.in
│   ├── com.github.vrnvctss.framework.metainfo.xml.in
│   ├── com.github.vrnvctss.framework.gschema.xml
│   └── icons/
│       └── hicolor/
│           ├── scalable/apps/com.github.vrnvctss.framework.svg
│           └── symbolic/apps/com.github.vrnvctss.framework-symbolic.svg
├── flatpak/
│   └── com.github.vrnvctss.framework.yml
└── po/                         # i18n (optional for v1, but structure it now)
    └── POTFILES.in
```

### 8.2 Naming Conventions

- GObject type prefix: `Fw` (e.g., `FwApplication`, `FwWindow`, `FwDocument`)
- C function prefix: `fw_` (e.g., `fw_application_new()`, `fw_document_open()`)
- File prefix: `fw-` (e.g., `fw-application.c`)
- GType macro: `FW_TYPE_APPLICATION`, etc.

---

## 9. Flatpak Distribution

Primary distribution method. Framework should be Flatpak-first.

### 9.1 Manifest Considerations

- Runtime: `org.gnome.Platform` / `org.gnome.Sdk` (version matching GNOME 50 target)
- MuPDF: bundled as a module (not in GNOME runtime). Build from source with `-DMUPDF_SHARED=ON` or link statically
- DjVuLibre: bundled as a module (not in GNOME runtime)
- Permissions: minimal. Needs filesystem access for opening files (via portal), nothing else
- Portals: use `org.freedesktop.portal.FileChooser` for file open, `org.freedesktop.portal.Print` for printing

### 9.2 AppStream Metadata

Provide `com.github.vrnvctss.framework.metainfo.xml` with:
- App name, summary, description
- Screenshots
- Release notes
- Content rating (OARS: none — it's a document viewer)
- Categories: Viewer, Office

---

## 10. Printing

Use GTK's native print infrastructure:

1. `GtkPrintOperation` handles the print dialog and platform integration
2. In the `draw-page` signal handler, render the requested page via the document backend directly to the print context's cairo surface
3. Set page count from document page count
4. Support page range selection, copies, orientation — all handled by GtkPrintOperation natively

No export-to-PDF (the document already is a PDF). No "save as" anything.

---

## 11. Invert Colors

Simple color inversion for reading in dark environments:

- Invert the rendered cairo surface pixel data (bitwise NOT on RGB channels, preserve alpha)
- Applied at the rendering/display stage, not at the document level
- Toggle via menu or `Ctrl+I`
- State saved in preferences (global) not per-document

---

## 12. Error Handling

| Condition | Behavior |
|-----------|----------|
| File not found | `AdwMessageDialog` with error message |
| Unsupported format | `AdwMessageDialog`: "Framework cannot open this file type" |
| Corrupted document | Attempt to open; if MuPDF/DjVuLibre returns error, show `AdwMessageDialog` with the error details |
| Render failure (single page) | Show error placeholder for that page, continue rendering others |
| Out of memory (pre-cache) | Degrade gracefully: stop pre-caching remaining pages, render on-demand. Log warning |

---

## 13. What Framework Is Not

Explicitly out of scope for v1.0 and likely forever:

- **Not a file manager.** No recent files, no library, no collections, no thumbnails grid
- **Not an editor.** No annotations, no form filling, no signatures, no markup
- **Not a converter.** No export, no save-as, no format conversion
- **Not a browser.** No tabs, no multi-document management within a single window
- **Not an image viewer.** No JPEG, PNG, TIFF, SVG support
- **Not an ebook reader.** No EPUB, no MOBI, no reflow

---

## 14. Future Considerations (v1.1+, not v1.0)

These are explicitly deferred. Do not implement in v1.0. Listed here only to ensure the architecture doesn't preclude them:

- **Thumbnail sidebar** (alternative sidebar mode alongside TOC)
- **Annotations** (highlight, underline — stored externally, not modifying the document)
- **Presentation mode** (page-at-a-time, no chrome, slide-show style)
- **Additional formats** via MuPDF (EPUB, XPS, CBZ) — only if there's demand
- **Smooth zoom** (pinch-to-zoom on touchscreens)
- **Configurable keybindings** (via GSettings, not a priority)

---

## 15. Success Criteria

Framework v1.0 is done when:

1. Opens a 500-page PDF and reaches full scroll-without-stutter in under 5 seconds on a mid-range machine (Ryzen 5 / 16GB RAM)
2. DjVu files open and render correctly
3. All UI controls described in this spec are present and functional
4. All keyboard shortcuts work
5. Per-document state persists across sessions
6. Search finds text across all pages with match highlighting
7. TOC sidebar populates and navigates correctly
8. Prints via system print dialog
9. Packages as a Flatpak and installs cleanly
10. A grandma can open a PDF and read it without asking for help
