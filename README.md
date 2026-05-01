<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

<p align="center">
  <img src="logo.svg" alt="Framework" width="420">
</p>

<p align="center">
  <a href="https://www.gtk.org/"><img src="https://img.shields.io/badge/GTK4-libadwaita-4a86cf" alt="GTK4"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--3.0-blue.svg" alt="License: GPL-3.0"></a>
  <a href="https://ko-fi.com/vrnvctss"><img src="https://img.shields.io/badge/support-Ko--fi-ff5f5f?logo=kofi" alt="Ko-fi"></a>
</p>

This is very much a work-in-progress and is not feature-complete. I'm a computer science student playing with AI on this one, so it's not a big priority, but it currently renders perfectly fine.

---

# Framework

A fast, native GNOME document viewer built on MuPDF, DjVuLibre, and libarchive. Framework opens **PDF**, **DjVu**, **CBZ**, **CBR**, **XPS**, **EPUB**, **FB2**, and **MOBI** documents, and is engineered for performance — utilizing aggressive pre-caching and a modern libadwaita UI to provide a "SumatraPDF-like" experience for Linux.

## Why this exists

Linux document viewers often fall into two categories: feature-heavy clients (like Okular) that bring extensive dependencies to GNOME, or minimal MuPDF wrappers that lack a functional UI. Framework fills the gap by providing a native, high-performance GNOME solution that prioritizes rendering speed and kinetic scrolling without the bloat of an editor.

## Features

| Feature | Description |
|---------|-------------|
| **Velocity Engine** | Render queue dispatch driven by scroll velocity, with mid-render `fz_cookie` abort on PDFs so workers can bail in milliseconds when the user has already moved on. |
| **Three-Tier Cache** | Persistent thumbnails, parsed page handles, and rendered surfaces with bytes-aware eviction (default 512 MB cap, tunable via `FW_CACHE_BYTES_CAP_MB`). |
| **Sort-Function Priority Dispatch** | `g_thread_pool_set_sort_function` reorders the render queue by `last_view_time` so the most recently prioritized page runs next. The viewport always wins. |
| **Parallel Rendering** | Eight independent MuPDF instances render pages across multiple CPU cores with zero shared state. |
| **Zero-Copy Render** | MuPDF and DjVuLibre both write rendered pixels straight into the cairo surface buffer — no intermediate pixmap, no channel shuffle. |
| **HiDPI Scaling** | Native device pixel ratio rendering for sharp text on Wayland fractional scaling. |
| **Async Search with Cached Stext** | Page-by-page scan on a worker thread, surface hits as found. The 5-figure-page-textbook case warms once (~330 ms) and serves every subsequent search from cached structured text in tens of ms. |
| **Smart Text Selection** | Double-click selects a word, triple-click selects a line. Drag selection follows reading order across line wraps with per-line highlight rectangles. |
| **Auto-Reload** | `GFileMonitor` watches the open document — recompile your LaTeX or Typst doc and Framework refreshes automatically, restoring exact scroll position. |
| **Document Properties** | Per-document metadata dialog (title, author, dates, format, page count, file size) backed by a `get_metadata` interface method. |

## Screenshot

<p align="center">
  <img src="https://github.com/user-attachments/assets/4f2a77a5-76f1-4d56-8238-a3190bb1be2e" alt="Framework Screenshot" style="max-width: 100%; border-radius: 8px;">
</p>

## Development & Build

### Requirements
- `gtk4` (4.16+), `libadwaita` (1.7+)
- `mupdf` (1.24+), `djvulibre` (3.5.28+)
- `meson` (1.4+)

### Build Pipeline
```bash
meson setup builddir
meson compile -C builddir
```

## What Framework is not

Framework is strictly a **viewer**. It is not an editor (no annotations), not a library manager, and not an image viewer. It focuses on doing one thing exceptionally well: opening and displaying documents.

## Keyboard shortcuts

### Navigation

| Action | Shortcut |
|--------|----------|
| Next page | Page Down |
| Previous page | Page Up |
| First page | Home, Ctrl+Home |
| Last page | End, Ctrl+End |
| Go to page | Ctrl+G |
| Back (history) | Alt+Left |
| Forward (history) | Alt+Right |

### Zoom

| Action | Shortcut |
|--------|----------|
| Zoom in | Ctrl+Plus, Ctrl+=, Ctrl+Scroll Up |
| Zoom out | Ctrl+Minus, Ctrl+Scroll Down |
| Fit width | Ctrl+1 |
| Fit page | Ctrl+2 |
| Actual size (100%) | Ctrl+0 |

### View

| Action | Shortcut |
|--------|----------|
| Toggle sidebar | F9 |
| Fullscreen | F11 |
| Invert colors | Ctrl+I |
| Reading ruler | F8 |
| Magnifying loupe | F7 |

### Search

| Action | Shortcut |
|--------|----------|
| Find | Ctrl+F |
| Next match | F3 |
| Previous match | Shift+F3 |
| Close search | Escape |

### General

| Action | Shortcut |
|--------|----------|
| Open file | Ctrl+O |
| Print | Ctrl+P |
| Copy selected text | Ctrl+C |
| Quit | Ctrl+Q, Ctrl+W |

You can also drop a file directly onto the window to open it.

## Requirements

**Build dependencies:**

| Dependency | Purpose |
|------------|---------|
| gtk4 (4.16+) | UI toolkit |
| libadwaita (1.7+) | GNOME design patterns |
| mupdf (1.24+) | PDF / CBZ / XPS / EPUB / FB2 / MOBI rendering |
| djvulibre (3.5.28+) | DjVu rendering |
| libarchive (3.6+) | CBR (RAR) decompression |
| cairo (1.18+) | Surface management |
| glib (2.82+) | Data structures, threading |
| json-glib (1.10+) | State persistence |
| meson (1.4+) | Build system |

On Fedora:

```bash
sudo dnf install gtk4-devel libadwaita-devel mupdf-devel djvulibre-devel \
                 libarchive-devel cairo-devel glib2-devel json-glib-devel \
                 meson gcc
```

## Building

We standardize on `builddir` as the output directory. Do not use `build` to avoid confusion.

```bash
meson setup builddir
meson compile -C builddir
```

### Flatpak (sandboxed)

A Flatpak manifest at the project root builds Framework against `org.gnome.Platform//50` with bundled MuPDF and DjVuLibre. Install locally with:

```bash
flatpak install flathub org.gnome.Platform//50 org.gnome.Sdk//50
flatpak-builder --user --install --force-clean build-flatpak io.github.virinvictus.framework.yml
flatpak run io.github.virinvictus.framework
```

The sandbox has no network access, no broad filesystem access, and uses the Document portal for arbitrary file picks — `xdg-documents`, `xdg-download`, and `xdg-desktop` are reachable directly for command-line invocations.

## Usage

```bash
# Open a PDF
framework document.pdf

# Open a DjVu file
framework book.djvu

# Open a CBZ comic-book archive
framework volume.cbz
```

CBR archives are handled via `libarchive` (BSD-licensed), so RAR-compressed comics open natively without the libunrar licensing trap. EPUB pagination is whatever MuPDF's default layout produces — Framework is good for fixed-layout EPUBs and as an "open everything" reader; serious EPUB readers like [Foliate](https://johnfactotum.github.io/foliate/) handle reflow and font customization better.

One document per window. Multiple files open multiple windows.

## What Framework is not

- **Not an editor.** No annotations, no form filling, no signatures
- **Not a converter.** No export, no save-as, no format conversion
- **Not a file manager.** No recent files, no library, no collections
- **Not a browser.** No tabs, no multi-document within a single window
- **Not an image viewer.** No JPEG, PNG, TIFF, SVG support

## Influences and borrowed techniques

Framework is a study in standing on shoulders. The following projects were studied closely and specific techniques are borrowed from each — landed today, scheduled in the roadmap, or under evaluation. Per-technique attribution lives here; per-source-file attributions stay in our SPDX headers.

### [SumatraPDF](https://www.sumatrapdfreader.org/) &mdash; *spiritual foundation*
Copyright © 2006–2024 the SumatraPDF project authors. Licensed [GPL-3.0](https://github.com/sumatrapdfreader/sumatrapdf/blob/master/COPYING).

Sumatra's "just a viewer" philosophy &mdash; uncompromising performance, no editor features, no library manager, every action visible &mdash; is the design north star for Framework. Specific techniques studied:

- **Engine abstraction layer** (`src/EngineBase.h`, `EngineMupdf.cpp`) &mdash; the shape of our `FwDocument` interface follows the same pattern.
- **Render-cache state machine** (`src/RenderCache.cpp`) &mdash; per-thread `curReqs[]` with abort cookies, semaphore-driven worker dispatch, and the "promote duplicate request to head of queue" trick informed `fw-cache.c`.
- **In-flight render cancellation via `fz_cookie`** (shipped in v0.17.0; see `src/fw-document-pdf.c:pdf_cancel_render`) &mdash; per-render `fz_cookie` published under a separate `cookies_lock` mutex so cancel never blocks on the worker; `fz_run_page` sees `cookie->abort = 1` and bails at its next checkpoint. Pattern from `EngineMupdf.cpp:3178`.
- **Bytes-aware cache cap** (shipped in v0.16.0; see `src/fw-cache.c`) &mdash; `total_cached_bytes` tracking + 512 MB default `byte_cap` with `FW_CACHE_BYTES_CAP_MB` override, replacing the page-count window. Pattern from `RenderCache.cpp:178` (`FreeIfFull`).
- **Auto-reload on file change** (shipped in v0.21.0; see `src/fw-window.c`) &mdash; `GFileMonitor`-driven swap with state-restore, the LaTeX/Typst killer feature. Conceptual pattern from Sumatra's `FileWatcher.cpp` reimagined in GIO idioms.
- **Smart text selection** (shipped in v0.19/v0.20; see `src/fw-document-pdf.c:pdf_select_at` and `pdf_get_selection_quads`) &mdash; double-click word, triple-click line, per-line drag highlights via `fz_snap_selection` + `fz_highlight_selection`. Pattern adapted from `TextSelection.cpp` `FindClosestGlyph` / `SelectWordAt`.
- **Scheduled borrows** (roadmap Phase 11+): tile-rendering as a high-zoom fallback; opportunistic per-page text extraction during render (the v0.18 stext cache populates lazily, not during render).

### [Zathura](https://pwmt.org/projects/zathura/) and [zathura-pdf-mupdf](https://pwmt.org/projects/zathura-pdf-mupdf/) &mdash; *render pipeline*
Copyright © 2009–2024 pwmt.org. Licensed [Zlib](https://github.com/pwmt/zathura/blob/develop/LICENSE).

zathura's zero-copy `MuPDF`&rarr;`cairo` pipeline is the textbook minimalist implementation. Specific techniques in Framework today and planned:

- **Zero-copy MuPDF render** (shipped in v1.6.0 as the v1.6 *Zero-Copy MuPDF Render* patch note; see `src/fw-document-pdf.c:render_page_direct`) &mdash; constructs the MuPDF pixmap *around* the cairo surface buffer via `fz_new_pixmap_with_bbox_and_data` + `fz_device_bgr`, eliminating the channel-shuffle loop entirely. Borrowed verbatim in pattern from `zathura-pdf-mupdf/render.c`.
- **Cached `fz_stext_page` per page** (shipped in v0.18.0; see `src/fw-document-pdf.c:pdf_get_or_extract_stext`) &mdash; lazy per-document cache, populated on first text-related call. Search across 901 pages: 332 ms cold &rarr; 48 ms warm (6.85&times; speedup). Pattern from `zathura-pdf-mupdf/page.c`.
- **`g_thread_pool_set_sort_function` priority dispatch** (shipped in v0.14.0; see `src/fw-cache.c:render_job_compare`) &mdash; the pool reorders queued jobs by `last_view_time` so the most recently prioritized page runs next. Pattern from `zathura/render.c:94`.
- **Hue-preserving recolor** (shipped in v0.22.0; see `src/fw-view.c` snapshot path) &mdash; luminance-aware affine that flips the lightness axis while preserving each pixel's chromatic offset. Red diagrams stay red on a dark background. Conceptually similar to `zathura/render.c`'s `colorumax` HSL recolor, reduced to a 4×4 GSK matrix.

### [Sioyek](https://sioyek.info/) &mdash; *zoom transitions and async search*
Copyright © Ali Mostafavi. Licensed [GPL-3.0](https://github.com/ahrm/sioyek/blob/main/LICENSE).

Sioyek's PDF renderer is the most carefully tuned single-document Linux MuPDF reader we found.

- **Closest-zoom fallback during transitions** (planned, roadmap Phase 11) &mdash; when the requested zoom level isn't ready, return the nearest-zoom rendered surface and scale it for display while the exact render proceeds. Pattern from `pdf_renderer.cpp:try_closest_rendered_page`.
- **Async, progressive search worker** (shipped in v0.7.0; see `src/fw-search.c`) &mdash; dedicated worker thread that scans page-by-page and posts hits back to the main loop via signals as they're found, with generation-counter cancellation when the query changes. Pattern from `pdf_renderer.cpp:run_search` (which emits `search_advance` every 16 pages).
- **Slice-based rendering for huge pages** (planned, fallback only) &mdash; render in N&times;M slices when a single surface would exceed a memory threshold. Pattern from Sioyek's `(num_h_slices, num_v_slices)` per request.
- **Hybrid threading model** (under evaluation) &mdash; one parent `fz_context`, per-thread `fz_clone_context`, per-(thread, path) `fz_document` &mdash; sits between Sumatra's full-clone and Framework's 8-instance model on the memory/parallelism curve.

### [Plato](https://github.com/baskerville/plato) &mdash; *memory-pressure reference*
Copyright © 2017 Bastien Dejean. Licensed AGPL-3.0.

Plato runs MuPDF on Kobo e-readers (single-core ARM, ~256 MB RAM). **Technique reference only** &mdash; AGPL-3.0 is not source-compatible with our GPL-3-or-later, so no code is copied. Useful as a sanity check on memory-pressure decisions: Plato uses a 32 MB MuPDF store cap, half of Framework's per-instance budget. The discrepancy seeded roadmap Phase 11's "Per-instance MuPDF store size scaling" item.

### Rendering engines and runtime

- **[MuPDF](https://mupdf.com/)** &mdash; PDF and structured-text rendering. Copyright © Artifex Software. Licensed [AGPL-3.0](https://www.gnu.org/licenses/agpl-3.0.html).
- **[DjVuLibre](http://djvu.sourceforge.net/)** &mdash; DjVu rendering. Copyright © Léon Bottou et al. Licensed [GPL-2.0-or-later](https://djvu.sourceforge.net/COPYRIGHT.html).

We link these as system libraries; we do not vendor or copy their source.

### Platform

- [GTK](https://www.gtk.org/) and [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/) &mdash; UI toolkit. LGPL-2.1-or-later.
- [Cairo](https://www.cairographics.org/) &mdash; surface management. LGPL-2.1-or-later / MPL-1.1.
- [GLib](https://docs.gtk.org/glib/) and [JSON-GLib](https://gnome.pages.gitlab.gnome.org/json-glib/) &mdash; data structures, threading, JSON state. LGPL-2.1-or-later.

Thanks to the [GNOME](https://www.gnome.org/) project for the platform that makes this kind of single-purpose viewer possible at all.

## License

Framework's source code is licensed under the [GNU General Public License, version 3 or later](LICENSE).

Because Framework links against [MuPDF](https://mupdf.com/) (AGPL-3.0), the **shipping binary** is effectively AGPL-3.0 &mdash; redistributors must make corresponding source available. Framework's *source* remains GPL-3-or-later: when distributing source, recipients may choose any GPL version 3 or later.

When techniques from GPL-3 sources (SumatraPDF, Sioyek) are incorporated, the resulting combined work is distributable under GPL-3 (the common denominator); the original authors are credited in this README and our SPDX headers remain `GPL-3.0-or-later`.

## Support

Support me by donating bitcoin (even a coffee would help):  
bc1qkge6zr45tzqfwfmvma2ylumt6mg7wlwmhr05yv
