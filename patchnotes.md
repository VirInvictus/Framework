# Framework — Patch Notes

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
