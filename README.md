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

A fast, native GNOME document viewer built on MuPDF and DjVuLibre. Framework is engineered for performance, utilizing aggressive pre-caching and a modern libadwaita UI to provide a "SumatraPDF-like" experience for Linux.

## Why this exists

Linux document viewers often fall into two categories: feature-heavy clients (like Okular) that bring extensive dependencies to GNOME, or minimal MuPDF wrappers that lack a functional UI. Framework fills the gap by providing a native, high-performance GNOME solution that prioritizes rendering speed and kinetic scrolling without the bloat of an editor.

## Features

| Feature | Description |
|---------|-------------|
| **Velocity Engine** | Dynamic cache management that throttles render jobs based on scroll speed. |
| **Two-Tier Cache** | Separates parsed page objects from rendered surfaces to minimize I/O. |
| **Parallel Rendering** | Independent MuPDF instances render pages across multiple CPU cores. |
| **Zero-Copy DjVu** | Full DjVuLibre support with zero-copy rendering into Cairo surfaces. |
| **HiDPI Scaling** | Native device pixel ratio rendering for sharp text on Wayland. |

## Screenshot

<p align="center">
  <img src="https://github.com/user-attachments/assets/4f2a77a5-76f1-4d56-8238-a3190bb1be2e" alt="DeaDBeeF CUI Plugin Screenshot" style="max-width: 100%; border-radius: 8px;">
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
| Find | Ctrl+F |
| Invert colors | Ctrl+I |

## Requirements

**Build dependencies:**

| Dependency | Purpose |
|------------|---------|
| gtk4 (4.16+) | UI toolkit |
| libadwaita (1.7+) | GNOME design patterns |
| mupdf (1.24+) | PDF rendering |
| djvulibre (3.5.28+) | DjVu rendering |
| cairo (1.18+) | Surface management |
| glib (2.82+) | Data structures, threading |
| json-glib (1.10+) | State persistence |
| meson (1.4+) | Build system |

On Fedora:

```bash
sudo dnf install gtk4-devel libadwaita-devel mupdf-devel djvulibre-devel \
                 cairo-devel glib2-devel json-glib-devel meson gcc
```

## Building

We standardize on `builddir` as the output directory. Do not use `build` to avoid confusion.

```bash
meson setup builddir
meson compile -C builddir
```

## Usage

```bash
# Open a PDF
framework document.pdf

# Open a DjVu file
framework book.djvu
```

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
- **Scheduled borrows** (roadmap Phase 11): in-flight render cancellation via `fz_cookie`; bytes-aware bitmap cache cap; tile-rendering as a high-zoom fallback; opportunistic per-page text extraction during render; double-click word selection (`TextSelection.cpp` `SelectWordAt`).

### [Zathura](https://pwmt.org/projects/zathura/) and [zathura-pdf-mupdf](https://pwmt.org/projects/zathura-pdf-mupdf/) &mdash; *render pipeline*
Copyright © 2009–2024 pwmt.org. Licensed [Zlib](https://github.com/pwmt/zathura/blob/develop/LICENSE).

zathura's zero-copy `MuPDF`&rarr;`cairo` pipeline is the textbook minimalist implementation. Specific techniques in Framework today and planned:

- **Zero-copy MuPDF render** (shipped in v1.6.0 as the v1.6 *Zero-Copy MuPDF Render* patch note; see `src/fw-document-pdf.c:render_page_direct`) &mdash; constructs the MuPDF pixmap *around* the cairo surface buffer via `fz_new_pixmap_with_bbox_and_data` + `fz_device_bgr`, eliminating the channel-shuffle loop entirely. Borrowed verbatim in pattern from `zathura-pdf-mupdf/render.c`.
- **Cached `fz_stext_page` per parsed page** (planned, roadmap Phase 11) &mdash; build the structured-text page once at parse time, reuse for both selection and search. Pattern from `zathura-pdf-mupdf/page.c`.
- **`g_thread_pool_set_sort_function` priority dispatch** (planned) &mdash; let the pool itself reorder pending jobs by `last_view_time` instead of walking a priority list. Pattern from `zathura/render.c:94`.
- **Hue-preserving recolor** (planned) &mdash; the `colorumax` HSL pipeline that preserves diagram color cues during dark-mode inversion, instead of a destructive bitwise-NOT. Pattern from `zathura/render.c`.

### [Sioyek](https://sioyek.info/) &mdash; *zoom transitions and async search*
Copyright © Ali Mostafavi. Licensed [GPL-3.0](https://github.com/ahrm/sioyek/blob/main/LICENSE).

Sioyek's PDF renderer is the most carefully tuned single-document Linux MuPDF reader we found.

- **Closest-zoom fallback during transitions** (planned, roadmap Phase 11) &mdash; when the requested zoom level isn't ready, return the nearest-zoom rendered surface and scale it for display while the exact render proceeds. Pattern from `pdf_renderer.cpp:try_closest_rendered_page`.
- **Async, progressive search worker** (planned) &mdash; dedicated thread that scans page-by-page using cached structured text, emitting progress every N pages. Pattern from `pdf_renderer.cpp:run_search`.
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
