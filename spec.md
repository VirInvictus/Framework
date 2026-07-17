# Framework — Application Specification

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

**Spec revision:** 4 (2026-07-16, tracks v0.80.0)
**Target:** Wayland Linux (Hyprland-first, GNOME-compatible), GTK4
**Language:** C (C17)
**Build System:** Meson
**License:** GNU GPL v3.0 or later

---

## Influence
Framework is heavily influenced by **SumatraPDF**'s philosophy: extreme performance, minimal UI, and a strict focus on being a viewer rather than an editor. It aims to be the "SumatraPDF for Linux".

## 1. Mission Statement

Framework is a fast, native Linux document viewer built on MuPDF, DjVuLibre, and libarchive. It renders **PDF, DjVu, EPUB, MOBI, FB2, XPS, and comic-book archives (CBZ, CB7, CBT, CBR)** with aggressive pre-caching, a clean plain-GTK4 UI under an owned stylesheet, and zero bloat. It is a viewer — not an editor, not a library manager, not a file organizer. It opens documents, displays them beautifully, and stays out of the way.

Reflowable formats (EPUB / FB2 / MOBI) get a fixed `fz_layout_document(600, 900, 11)` pass per render-instance open and do not re-flow on zoom or window resize — Framework is good as a single reader for "open everything," but specialized ebook readers (e.g., [Foliate](https://johnfactotum.github.io/foliate/)) handle reflow and font customization better.

Design philosophy: **accessible to a grandma, useful to a power user.** Every action has a visible UI control. Every UI control has a keyboard shortcut. No vim bindings, no modal interfaces, no hidden commands. SumatraPDF is the reference implementation.

### Design language (v0.80.0: de-adwaita, Hyprland-first)

Framework dropped libadwaita at v0.80.0 while keeping GTK4, to belong on a tiling Wayland desktop (Hyprland) rather than read as a GNOME Shell app. It stays fully functional under GNOME; only the look and a few affordances changed. The visual language matches the sibling projects (Hermitage, Colophon):

- **Owned stylesheet.** A single `data/style.css` is the styling authority, loaded via a `GtkCssProvider` at `GTK_STYLE_PROVIDER_PRIORITY_USER + 1` (above a user's `~/.config/gtk-4.0/gtk.css`). Flat, square, 1px hard borders, no shadows, denser spacing than adwaita. Kanagawa Dragon (dark) / Kanagawa Lotus (light) palette as `@define-color` named colours.
- **Dark/light** follows the system via the `org.freedesktop.portal.Settings` `color-scheme` preference read over GDBus (no GNOME dependency), with live updates on `SettingChanged` and a **dark default** when no portal backend answers. Fixed reading themes (Light / Sepia / Kanagawa Dark) still force their own polarity; only the "system" reading theme tracks the portal.
- **Titlebar.** A slim flat `GtkHeaderBar` promoted to the real window titlebar, with `show-title-buttons` off (the compositor and a `Ctrl+Q` quit accel handle window control). Consequence: GTK auto-hides the titlebar in fullscreen (F11), which suits a reader; the header's page/zoom entries are unreachable there, same as any GTK app in fullscreen.
- **TOC sidebar** floats: a `GtkRevealer` inside a `GtkOverlay` over the document, toggled by F9, dismissed by clicking outside. It never squeezes the page — deliberate for narrow tiles. Width persists in the `sidebar-width` GSetting.
- **Dialogs** (Reading Settings, Keyboard Shortcuts, Document Properties) are transient modal `GtkWindow`s built from owned `GtkListBox` "boxed-list" row composites, Escape-to-close.

---

## 2. Architecture

### 2.1 Rendering Abstraction

Framework uses a backend abstraction layer to support multiple document formats through different rendering libraries. Three backends share one interface (`FwDocument`):

```text
┌─────────────────────────────────────┐
│          FwDocument                 │
│  (abstract interface / vtable)      │
├─────────────────────────────────────┤
│  open / close                       │
│  get_page_count / get_page_size     │
│  render_page (full path)            │
│  open_page / close_page /           │
│      render_page_from_handle        │
│      (parsed-handle path for cache) │
│  cancel_render                      │
│  get_toc / get_links                │
│  search / get_text                  │
│  get_attachments / save_attachment  │
│  get_metadata                       │
└──┬──────────────┬──────────────┬────┘
   │              │              │
┌──┴──────┐  ┌────┴─────┐  ┌─────┴─────┐
│ MuPDF   │  │ DjVuLibre│  │ libarchive│
│ Backend │  │ Backend  │  │ Backend   │
│         │  │          │  │ (CBR)     │
└─────────┘  └──────────┘  └───────────┘
```

**MuPDF backend (`fw-document-pdf.c`).** Links against `libmupdf`. Despite the file name, this is the *MuPDF* backend, not specifically the PDF backend — `fz_register_document_handlers` + `fz_open_document` dispatch internally by content. It handles **PDF, CBZ, CB7, CBT, XPS** as the fixed-layout primary, and **EPUB / FB2 / MOBI** only as a *fallback* (those reach the WebKit reflow pipeline first; see §2.4). The render path is zero-copy into the cairo surface buffer via `fz_new_pixmap_with_bbox_and_data` + `fz_device_bgr` (v1.6 technique borrowed from zathura-pdf-mupdf). For comic formats it normalizes per-page sizes against inconsistent embedded DPI (v0.70).

**DjVuLibre backend (`fw-document-djvu.c`).** Links against `libdjvu` (ddjvuapi). Handles DjVu files via DjVuLibre's own page rendering, with `DDJVU_FORMAT_RGBMASK32` matched to cairo ARGB32 layout for zero-copy writes. Single mutex for the API; abort queue keeps `ddjvuapi` from CPU-locking under high-velocity scrubbing.

**libarchive backend (`fw-document-cbr.c`).** Links against `libarchive` (BSD-licensed; no `libunrar` licensing trap). Handles CBR archives (and any RAR/7z/tar of images by virtue of libarchive's format support). Render path: extract entry bytes → `fz_new_image_from_buffer` → `fz_fill_image` into a draw device wrapping the cairo surface buffer (the same v1.6 zero-copy pattern). Single mutex per archive — libarchive readers can't be safely shared across threads, and the streaming-RAR cost makes per-render archive opens dominate anyway.

**Backend selection.** The window first checks `fw_reflow_path_is_supported` (EPUB / MOBI / AZW3 / FB2 / TXT / Markdown); those open through the WebKit **reflow** pipeline (§2.4) and only fall back to MuPDF if the reflow parser refuses the file. Otherwise the fixed-layout factory picks by extension: `.pdf` / `.cbz` / `.cb7` / `.cbt` / `.xps` / `.oxps` → MuPDF; `.djvu` / `.djv` → DjVuLibre; `.cbr` → libarchive.

### 2.4 Reflow pipeline (WebKitGTK, Phase 17)

Reflowable formats are converted to a single stitched HTML document and rendered in a `WebKitWebView` (`fw-webview.c`), the web-engine strategy Calibre's viewer and Foliate both use. The `FwReflowDocument` backends parse each format and emit HTML via `produce_html`: EPUB/MOBI/AZW3 (foliate-js-derived parsers), FB2 (libxml2 walker), TXT (paragraph splitter), Markdown (vendored md4c, GitHub dialect). A shared module (`fw-reflow-html.c`) supplies the reading stylesheet and the `<img>`→`framework-img:` rewrite. Typography (serif default, font size, line height) and themes (Light / Sepia / Kanagawa-Dragon-Dark / Follow-System) are CSS custom properties pushed live onto the document `:root`. This replaced the earlier native-GTK block-model renderer (`FwReflowView`), removed in v0.76.

### 2.2 The Velocity-Driven Cache Engine

The pre-cache engine is the core performance differentiator. To balance rapid scrolling against memory boundaries and CPU thermal constraints, Framework uses a **Three-Tier, Velocity-Driven Architecture**, with two generation counters governing invalidation and abort separately.

#### Tier 0: Persistent Thumbnails (v1.5)
~150-px-wide previews rendered on a dedicated low-priority `GThreadPool`, stored for the document's lifetime, **never evicted**. ~120 KB per page → a 1000-page document costs ~120 MB. Used as the placeholder layer when a visible page has no full-resolution surface ready (fast scroll, cold cache, mid-zoom transition). Users see actual content during fast scroll instead of grey rectangles.

#### Tier 1: Parsed Window
~30-page window (reduced from 50 in v1.3.3 to lower speculative I/O) where the backend's parsed page handle is loaded (MuPDF `fz_page` or DjVu equivalent), but **no pixels are rendered**. Eliminates disk I/O when navigating, at a negligible RAM cost.

#### Tier 2: Pixel Window (Surface Cache)
A dynamic, strictly managed hash table of `cairo_surface_t` + cached `GdkTexture` pairs (the texture is reused across snapshot frames per v1.5). Eviction is dictated by the user's kinetic scroll velocity (`dy/dt`), calculated via `gtk_widget_add_tick_callback` on the scrollable view.

**Generation counters.** `render_gen` (param-change scope: zoom, rotation, scale) and `cancel_gen` (abort scope: scrubbing, stop) are split so scrubbing can abort in-flight work without invalidating correctly-rendered surfaces (v1.3.3). Both are `guint` counters checked inside worker jobs — no pthread cancellation.

**Velocity States & Strategies:**

1.  **Static (Velocity ≈ 0):** The user is reading.
    * Action: Render visible pages plus a small lookahead.
    * Pacing: Yield the thread pool. Let the CPU drop to idle.
2.  **Cruising (Moderate Velocity):** The user is reading at pace or scanning.
    * Action (current): visible + 7 forward + 3 backward, drip-feed renders.
    * Action (planned, see roadmap Phase 11 Tier 1): symmetric ±10 with sustained-velocity scroll damping so thumbnails are reserved for explicit jumps.
    * Pacing: Drip-feed background renders one at a time. The worker checks velocity between each render to prevent CPU spikes.
3.  **Scrubbing (High Velocity):** The user has grabbed the scrollbar or flicked the wheel hard.
    * Action: **ABORT.** Bump `cancel_gen`. In-flight workers see the bumped counter and bail. Paint thumbnails (Tier 0) while scrubbing.
    * DjVu / CBR constraint: strictly enforced for the single-mutex backends so they don't CPU-lock decoding skipped pages.
4.  **View Changes:** On zoom or rotation, bump `render_gen`. Stale surfaces move to a `prev_surface` slot per cache entry and the view paints them scaled-to-fit until the sharp re-render arrives — no grey flashes during zoom transitions (v1.4).

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

**Thread safety.** MuPDF is *not* thread-safe per-document. The PDF backend opens the file `MAX_RENDER_INSTANCES` (8) times, each with its own `fz_context` + `fz_document` + per-instance mutex; render threads round-robin across them. Cloned contexts share font/image stores but `fz_page` / `fz_image` lazy-read from streams owned by the document — concurrent reads on a shared document corrupt state even via display lists. DjVuLibre and libarchive both require serialized access via single-mutex workers.

**MuPDF exception handling** uses `setjmp` / `longjmp` via `fz_try` / `fz_catch`. **Never** `return` / `goto` / `longjmp` from inside those blocks. Variables modified in `fz_try` and read in `fz_catch` must be `volatile`. Use `fz_always` for cleanup.

### 2.3 Widget Tree

Plain GTK4 since v0.80.0 (libadwaita dropped); the shell is an owned
`GtkOverlay` with a floating TOC that never reallocates the page.

```text
GtkApplicationWindow (FwWindow)
├── [titlebar] GtkHeaderBar (show-title-buttons off; owned stylesheet)
│   ├── [start] GtkButton (sidebar toggle)
│   ├── [start] GtkButton (zoom out)
│   ├── [start] GtkEntry (zoom percentage, editable)
│   ├── [start] GtkButton (zoom in)
│   ├── [title] GtkLabel (filename)
│   ├── [end]   GtkMenuButton (primary menu)
│   ├── [end]   GtkToggleButton (search)
│   ├── [end]   GtkButton (next page)
│   ├── [end]   GtkButton (previous page)
│   └── [end]   GtkEntry (page number / total, editable)
└── GtkOverlay (root_overlay)
    ├── [child]   GtkStack ("empty" | "document" | "webview")
    │   ├── document: GtkScrolledWindow → FwView (custom widget, renders pages)
    │   ├── webview:  FwWebView (WebKitGTK reflow renderer)
    │   └── empty:    owned centered status box
    ├── [overlay] GtkRevealer (floating TOC, F9, click-outside dismiss)
    │   └── GtkScrolledWindow → FwSidebar / FwReflowSidebar (GtkListView)
    ├── [overlay] GtkRevealer (bottom-center toast) → GtkLabel
    └── [overlay] GtkSearchBar
        └── GtkSearchEntry + match count + prev/next buttons
```

### 2.4 Application Object

```text
FwApplication : GtkApplication
├── Handles file open via command line args and GtkFileDialog
├── Single-instance (activate brings existing window forward)
├── No tabs, no multi-document — one document per window
│   Multiple files = multiple windows (via g_application_open)
├── Stores per-file state in XDG_DATA_HOME/framework/state.json:
│   { "/path/to/file.pdf": { "page": 42, "zoom": 1.5, "scroll_y": 0.73 } }
│   LRU-pruned (max 500 entries; entries >90 days dropped on startup).
└── GSettings schema (`io.github.virinvictus.framework`) carries the
    real keys (reading theme/font/size/line-height, publisher-styles,
    kinetic-scrolling, reading-ruler, loupe, crop-margins, comic layout).
    Compiled in-tree via `gnome.compile_schemas` (a Meson helper; the
    only "gnome" left in the build, unrelated to the GNOME desktop).
```

---

## 3. UI Specification

### 3.1 Header Bar

Slim flat `GtkHeaderBar` promoted to the window titlebar (v0.80.0; was `AdwHeaderBar`), window-control buttons hidden, styled by the owned stylesheet. See "Design language" above.

**Left cluster — Sidebar + Zoom:**

| Control | Type | Behavior |
|---------|------|----------|
| Sidebar toggle | GtkButton | Toggles the floating TOC sidebar (F9). Icon: `view-sidebar-symbolic` |
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

A floating `GtkRevealer` inside the root `GtkOverlay` (v0.80.0; was `AdwOverlaySplitView`): it slides in over the content and is dismissed by clicking outside, so it never reallocates the page. This is deliberately tiling-friendly: a narrow tile is never squeezed side by side. Populated from the document's outline/TOC structure.

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

A `GtkSearchBar` overlaid on the content area.

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
- GTK auto-hides the titlebar in fullscreen, revealed on mouse movement to the top edge
- Search bar remains accessible via `Ctrl+F`
- Escape exits fullscreen

### 3.7 No Welcome Screen

If launched without a file argument, show an empty window with a centered "Open a Document" button or just the header bar with the Open action available. No recent files grid, no library view, no tips. Minimal.

---

## 4. Keyboard Shortcuts

Sumatra defaults adapted to GTK conventions. All shortcuts visible in the Keyboard Shortcuts dialog (`Ctrl+?`).

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

Two pipelines: **fixed-layout** (rasterized pages through the velocity cache) and **reflow** (converted to HTML and rendered in a `WebKitWebView`, Phase 17).

| Format | Backend | Pipeline | Extensions |
|--------|---------|----------|------------|
| PDF | MuPDF (libmupdf) | fixed-layout | `.pdf` |
| XPS | MuPDF | fixed-layout | `.xps`, `.oxps` |
| DjVu | DjVuLibre (libdjvu) | fixed-layout | `.djvu`, `.djv` |
| Comics (ZIP/7z/tar) | MuPDF | fixed-layout | `.cbz`, `.cb7`, `.cbt` |
| Comics (RAR) | libarchive | fixed-layout | `.cbr` |
| EPUB | foliate-js-derived parser → WebKitGTK | reflow | `.epub` |
| MOBI / AZW3 | foliate-js-derived parser → WebKitGTK | reflow | `.mobi`, `.prc`, `.azw`, `.azw3` |
| FB2 | libxml2 walker → WebKitGTK | reflow | `.fb2`, `.fb2.zip` |
| TXT | paragraph splitter → WebKitGTK | reflow | `.txt` |
| Markdown | md4c (vendored) → WebKitGTK | reflow | `.md`, `.markdown` |

Reflow formats fall back to MuPDF's fixed-layout rasterization if the reflow parser refuses the file. The MuPDF reflow path is the fallback, not the default (Phase 17).

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

Stored via GSettings. Schema: `io.github.virinvictus.framework` (adjust namespace as appropriate).

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
| gtk4 | 4.16+ | UI toolkit (plain GTK4; libadwaita dropped in v0.80.0, the app owns its stylesheet) |
| mupdf | 1.24+ | PDF / XPS / comic rendering |
| djvulibre | 3.5.28+ | DjVu rendering |
| libarchive | 3.6+ | CBR (RAR) decompression |
| webkitgtk-6.0 | 2.46+ | Reflow rendering (EPUB/MOBI/AZW3/FB2/TXT/Markdown) |
| libxml2 | 2.x | HTML/XML parsing for the reflow + FB2 backends |
| fontconfig | 2.x | Registering the bundled reading fonts |
| cairo | 1.18+ | Surface management, compositing |
| glib | 2.82+ | Data structures, threading, I/O |
| json-glib | 1.10+ | State persistence |
| meson | 1.4+ | Build system |
| md4c | vendored (MIT) | Markdown→HTML; bundled in `src/md4c/`, no system dep |

### Runtime Dependencies

All of the above as shared libraries. The Flatpak builds against the `org.gnome.Platform` runtime, which is simply the standard GTK4 Flatpak runtime; Framework links plain GTK4, not libadwaita, and does not depend on a GNOME session at runtime.

---

## 8. Build Configuration

### 8.1 Meson Project Structure

```text
framework/
├── meson.build                 # top-level (project version lives here)
├── io.github.virinvictus.framework.yml   # Flatpak manifest (root, per Flathub convention)
├── src/
│   ├── meson.build             # framework_sources list — no glob, add new files explicitly
│   ├── main.c                  # entry point, GtkApplication setup
│   ├── fw-application.c/h      # FwApplication (single-instance GtkApplication)
│   ├── fw-theme.c/h            # owned stylesheet + portal color-scheme reader
│   ├── fw-window.c/h           # FwWindow (GtkApplicationWindow)
│   ├── fw-view.c/h             # FwView (custom GtkScrollable, paints all visible pages)
│   ├── fw-cache.c/h            # Three-tier velocity-driven pre-cache engine
│   ├── fw-document.c/h         # Abstract FwDocument interface + factory
│   ├── fw-document-pdf.c/h     # MuPDF backend (PDF, CBZ/CB7/CBT, XPS; EPUB/FB2/MOBI fallback)
│   ├── fw-document-djvu.c/h    # DjVuLibre backend
│   ├── fw-document-cbr.c/h     # libarchive backend (CBR, plus any RAR/7z/tar of images)
│   ├── fw-webview.c/h          # WebKitWebView reflow renderer + framework-img: scheme
│   ├── fw-reflow-document.c/h  # FwReflowDocument interface + reflow factory
│   ├── fw-reflow-html.c/h      # Shared reading CSS + HTML emit helpers (reflow)
│   ├── fw-reflow-document-{epub,mobi,fb2,txt,md}.c/h  # per-format reflow backends
│   ├── fw-mobi-parser.c/h      # PalmDB/PalmDOC/KF7/KF8 byte-format parser
│   ├── md4c/                   # Vendored md4c (MIT): Markdown to HTML
│   ├── fw-sidebar.c/h          # TOC sidebar (GtkListView + GtkTreeListModel)
│   ├── fw-search.c/h           # Async search controller (fixed-layout)
│   ├── fw-fonts.c/h            # Registers bundled reading fonts with FontConfig
│   ├── fw-sandbox.c/h          # Landlock LSM hardening
│   ├── fw-state.c/h            # Per-document state persistence (LRU JSON)
│   └── fw-debug.c/h            # Runtime trace domains (FW_DEBUG=1 → timestamped logs)
├── data/
│   ├── meson.build
│   ├── io.github.virinvictus.framework.desktop.in
│   ├── io.github.virinvictus.framework.metainfo.xml.in
│   ├── io.github.virinvictus.framework.gschema.xml
│   └── icons/
│       └── hicolor/
│           └── scalable/apps/io.github.virinvictus.framework.svg
│           # symbolic/apps/io.github.virinvictus.framework-symbolic.svg — TODO
└── po/                         # i18n scaffolding
    └── POTFILES.in
```

Future structure (per roadmap Phase 12, gated on `-Dstress=true`):
```text
├── meson_options.txt           # not present yet — added when Phase 12 lands
├── tests/                      # not present yet — added when Phase 12 lands
│   ├── corpus.json
│   ├── stress/  bench/  scripts/
```

### 8.2 Naming Conventions

- GObject type prefix: `Fw` (e.g., `FwApplication`, `FwWindow`, `FwDocument`)
- C function prefix: `fw_` (e.g., `fw_application_new()`, `fw_document_open()`)
- File prefix: `fw-` (e.g., `fw-application.c`)
- GType macro: `FW_TYPE_APPLICATION`, etc.

---

## 9. Flatpak Distribution

Primary distribution method. Framework is Flatpak-first. The manifest at the project root (`io.github.virinvictus.framework.yml`) builds and runs end-to-end. Local install workflow tested against `org.gnome.Platform//50` + `org.gnome.Sdk//50`.

### 9.1 Manifest Realized

- **Runtime:** `org.gnome.Platform//50` + `org.gnome.Sdk//50`
- **Modules:** `djvulibre` (autotools, `--disable-static --disable-desktopfiles`), `mupdf` (project Makefile, `HAVE_X11=no HAVE_GLUT=no HAVE_LIBCRYPTO=no shared=yes USE_SYSTEM_LIBS=no` — bundled third-party libs are simpler than runtime equivalents), `framework` (meson, release buildtype). `libarchive` comes from the freedesktop runtime under GNOME 50 — no module needed.
- **Permissions (`finish-args`):** no network, no broad filesystem. `--device=dri` for GPU. `--socket=wayland` + `--socket=fallback-x11`. Read-only `--filesystem=xdg-documents` / `--filesystem=xdg-download` / `--filesystem=xdg-desktop` for command-line invocations. Anything else reaches Framework via the Document portal automatically (GtkFileDialog and drag-and-drop both go through it).
- **Portals consumed:** `org.freedesktop.portal.FileChooser` (for file picks), `org.freedesktop.portal.Print` (for `Ctrl+P`), `org.freedesktop.portal.OpenURI` (for external link clicks via `GtkUriLauncher`). All are auto-included by the SDK's portal wiring; no explicit `--talk-name=` flags needed.

### 9.2 AppStream Metadata

`data/io.github.virinvictus.framework.metainfo.xml.in` (translated and merged at build time) ships:
- App name, summary, description (current format list, feature bullets)
- `<developer>` block, `<categories>` (Office, Viewer, GTK)
- `<recommends>` (display ≥ 600 px, offline-only network), `<supports>` (pointing/keyboard/touch)
- Release notes from v0.6.0 → current under honest versioning (the historical 1.x labels stay in `patchnotes.md` but are not surfaced to software centers)
- Content rating: OARS 1.1, default (no objectionable content)
- Screenshots: TODO before Flathub submission. The `<screenshots>` block sits commented in the metainfo as a template — once `data/screenshots/` exists with stable filenames, uncomment the block and update the GitHub raw URLs.

`appstreamcli validate` and `desktop-file-validate` must both pass before any release tag.

---

## 10. Printing

Use GTK's native print infrastructure:

1. `GtkPrintOperation` handles the print dialog and platform integration
2. In the `draw-page` signal handler, render the requested page via the document backend directly to the print context's cairo surface
3. Set page count from document page count
4. Support page range selection, copies, orientation — all handled by GtkPrintOperation natively

No export-to-PDF (the document already is a PDF). No "save as" anything.

---

## 11. Invert Colors (Hue-Preserving Recolor)

Dark-mode display for reading in low-light environments:

- **Luminance-aware affine transform** applied via `gtk_snapshot_push_color_matrix` — for each pixel compute BT.601 luma `Y = 0.299R + 0.587G + 0.114B`, then offset each channel by `(1 − 2Y)`. Equivalent to flipping the lightness axis while preserving the chromatic component (R−Y, G−Y, B−Y) of every pixel.
- White → near-black (background darkens), black → near-white (text lightens), red stays red on the new dark background, blue plots stay blue. Diagrams and syntax-highlighted code keep their meaning.
- Applied GPU-side at snapshot time, not by re-rendering surfaces — toggling is instant with no cache invalidation.
- Toggle via menu or `Ctrl+I`. State is per-window for the session; not persisted across launches in the current schema.
- Future: configurable `recolor-light` / `recolor-dark` GSettings keys for full theme customization (v0.22 hardcodes the standard white↔black mapping).

---

## 12. Error Handling

| Condition | Behavior |
|-----------|----------|
| File not found | `GtkAlertDialog` with error message |
| Unsupported format | `GtkAlertDialog`: "Framework cannot open this file type" |
| Corrupted document | Attempt to open; if MuPDF/DjVuLibre returns error, show `GtkAlertDialog` with the error details |
| Render failure (single page) | Show error placeholder for that page, continue rendering others |
| Out of memory (pre-cache) | Degrade gracefully: stop pre-caching remaining pages, render on-demand. Log warning |

---

## 13. What Framework Is Not

Explicitly out of scope for v1.0 and likely forever:

- **Not a file manager.** No recent files, no library, no collections, no thumbnails grid.
- **Not an editor.** No annotations, no form filling, no signatures, no markup.
- **Not a converter.** No export, no save-as, no format conversion.
- **Not a browser.** No tabs, no multi-document management within a single window. Multiple files = multiple windows.
- **Not an image viewer.** No standalone JPEG, PNG, TIFF, SVG support. (Comic-book archives are framed images-as-pages — that's a different use case.)
- **Not a full ebook reader.** As of Phase 17, EPUB / MOBI / AZW3 / FB2 / TXT / Markdown render through WebKitGTK with real reflow, a serif reading font, light/sepia/dark themes, font-size and line-height control, TOC navigation, and in-text search: a genuine reading experience, not just "it opens." What Framework still doesn't do: a library, annotations, dictionary/lookup, sync, or per-publisher CSS. For a dedicated ebook workflow [Foliate](https://johnfactotum.github.io/foliate/) remains more complete; Framework is the right tool when you want one viewer for fixed-layout PDFs, comics, and ebooks without switching apps.

---

## 14. Future Considerations (post-1.0 / v1.x)

These are explicitly deferred. Do not implement before 1.0. Listed here only to ensure the architecture doesn't preclude them. Phase status in `roadmap.md` is the source of truth — phases 11–14 detail the borrows, layout shifts, and UX polish targeted post-1.0.

- **Thumbnail sidebar** (alternative sidebar mode alongside TOC).
- **Annotations** (highlight, underline — stored externally, not modifying the document).
- **Presentation mode** (page-at-a-time, no chrome, slide-show style).
- **Single-page view mode** for general documents (the current default is continuous vertical scroll). Facing-pages, manga, and webtoon modes shipped in v0.27 — see Phase 13 in `roadmap.md`.
- **Smooth pinch-to-zoom** on touchscreens.
- **Configurable keybindings** via GSettings.
- ~~**Fractal-style EPUB reflow.**~~ **Shipped, then superseded.** The native-GTK block-model reflow renderer shipped in Phase 13.1 (v0.40+), then the Phase 17 pivot (v0.68+) replaced it with WebKitGTK and the block-model path was removed in v0.76. See §2.4 and roadmap Phase 17.
- ~~**Auto-reload on file change.**~~ Shipped v0.21 (`GFileMonitor`); fixed-layout only (the reflow/WebView path does not yet re-watch; a Phase 17.x follow-up).

---

## 15. Success Criteria

Framework v1.0 is done when all of the following hold. As of v0.21.0, only the release-mechanics items remain. The substantive work — cache architecture, all formats, polish — is shipped.

| Status | Criterion |
|---|---|
| ✅ | Opens a 500-page PDF and reaches scroll-without-stutter on a mid-range machine (Ryzen 5 / 16 GB RAM). |
| ✅ | DjVu files open and render correctly. |
| ✅ | CBZ / CB7 / CBT / CBR comic-book archives open and render correctly. |
| ✅ | EPUB / MOBI / AZW3 / FB2 / TXT / Markdown reflow through WebKitGTK with serif typography, themes, TOC, and search (Phase 17). |
| ✅ | All UI controls described in this spec are present and functional. |
| ✅ | All keyboard shortcuts work; the Keyboard Shortcuts dialog (`Ctrl+?` / `F1`) lists them. |
| ✅ | Per-document state persists across sessions, LRU-pruned. |
| ✅ | Search finds text across all pages with match highlighting; runs async without blocking the UI. |
| ✅ | TOC sidebar populates, navigates, and follows the current page during scroll. |
| ✅ | Prints via the system print dialog. |
| ✅ | Document Properties dialog displays available metadata. |
| ✅ | Packages as a Flatpak (sandboxed, portal-based file access, no network) and installs cleanly. |
| ✅ | Startup-blur regression on saved-state open is fixed (v0.14 + v0.17 sort-function + cookie work; verified). |
| ✅ | Continuous scroll never paints thumbnail placeholders during normal reading (v0.14 symmetric ±10 + v0.17 mid-render `fz_cookie` abort). |
| ✅ | Bytes-aware cache cap (v0.16) — per-surface byte tracking replaces the old fixed page-count window. |
| ✅ | Smart text selection (v0.19): double-click word, triple-click line; v0.20 per-line drag highlights. |
| ✅ | Auto-reload via `GFileMonitor` (v0.21) — recompile and the document refreshes with state restored. |
| ☐ | A `<screenshots>` block exists in the AppStream metainfo before any Flathub submission. |
| ☐ | Tagged `1.0.0`, signed if applicable. |
| ✅ | A grandma can open a PDF and read it without asking for help. |
