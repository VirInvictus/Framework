<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

<p align="center">
  <img src="logo.svg" alt="Framework" width="420">
</p>

<p align="center">
  <a href="https://www.gtk.org/"><img src="https://img.shields.io/badge/GTK4-libadwaita-4a86cf" alt="GTK4"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--3.0-blue.svg" alt="License: GPL-3.0"></a>
</p>

---

# Framework

A fast, native GNOME document viewer built on MuPDF, DjVuLibre, and libarchive. Framework opens **PDF**, **DjVu**, **CBZ**, **CBR**, **XPS**, **EPUB**, **FB2**, **MOBI**, and **AZW3** documents, and is engineered for performance — utilizing aggressive pre-caching and a modern libadwaita UI to provide a "SumatraPDF-like" experience for Linux.

## Why this exists

Linux document viewers often fall into two categories: feature-heavy clients (like Okular) that bring extensive dependencies to GNOME, or minimal MuPDF wrappers that lack a functional UI. Framework fills the gap by providing a native, high-performance GNOME solution that prioritizes rendering speed and kinetic scrolling without the bloat of an editor.

I'm not pretending I came up with the architecture. Framework is a deliberate synthesis: the cache + threading idioms of **SumatraPDF**, the zero-copy MuPDF→cairo pipeline and `GThreadPool` priority dispatch from **zathura-pdf-mupdf** / **zathura**, the magnifying loupe and double-spread detection from **YACReader**, and the native-GTK reflow architecture from **Foliate** (and its parser library **foliate-js**), all glued into a minimalist libadwaita UI for GNOME. Patterns are also borrowed from Sioyek, Plato, MComix, and Komikku where they had something specific to teach. Per-pattern attribution lives in the [Influences](#influences-and-borrowed-techniques) section below — every borrow is named with its upstream file:line and the Framework version it shipped in.

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
| **Comic Layouts** | Manga mode (RTL nav), Webtoon mode (zero-gap continuous strip), and Facing Pages (two-up with cover standalone) — composable, layout-anchor-preserving, and live-toggleable from the menu or F4/F5/F10. |
| **Native Ebook Reflow** | EPUB, FB2, MOBI, and AZW3 render as native GTK widgets (not rasterized pages) through a Foliate-style block model: real text wrapping, justification, first-line indent, chapter titles, lists, drop caps, and live font-size adjustment. Format parsers are C ports of foliate-js. Falls back to MuPDF fixed layout if a document won't parse. |

## Screenshot

<p align="center">
  <img src="https://github.com/user-attachments/assets/4f2a77a5-76f1-4d56-8238-a3190bb1be2e" alt="Framework Screenshot" style="max-width: 100%; border-radius: 8px;">
</p>

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
| Crop margins | F6 |

### Comic Layout

| Action | Shortcut |
|--------|----------|
| Manga mode (RTL) | F4 |
| Webtoon mode | F5 |
| Facing pages | F10 |

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
| mupdf (1.24+) | PDF / CBZ / XPS rendering (and fallback ebook layout) |
| djvulibre (3.5.28+) | DjVu rendering |
| libarchive (3.6+) | CBR (RAR) decompression and EPUB (ZIP) reading |
| libxml-2.0 (2.9+) | XHTML / FB2 / OPF parsing for the reflow parsers |
| webkitgtk-6.0 (2.46+) | Reflow rendering (EPUB now; MOBI / AZW3 / FB2 / TXT incrementally). Fixed-layout formats are unchanged. |
| fontconfig | Bundled-font registration for reflow text |
| cairo (1.18+) | Surface management |
| glib (2.82+) | Data structures, threading |
| json-glib (1.10+) | State persistence |
| meson (1.4+) | Build system |

On Fedora:

```bash
sudo dnf install gtk4-devel libadwaita-devel mupdf-devel djvulibre-devel \
                 libarchive-devel libxml2-devel webkitgtk6.0-devel \
                 fontconfig-devel cairo-devel glib2-devel json-glib-devel \
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

CBR archives are handled via `libarchive` (BSD-licensed), so RAR-compressed comics open natively without the libunrar licensing trap. EPUB, FB2, MOBI, and AZW3 open through Framework's native reflow pipeline: text wraps and reflows as native GTK widgets with real typography, live font-size adjustment, a chapter sidebar, and in-text search highlighting, rather than MuPDF's fixed layout (which remains as an automatic fallback for documents the reflow parser can't handle). Some dedicated-reader polish (a reading-progress indicator, for instance) is still in progress; [Foliate](https://johnfactotum.github.io/foliate/) remains the more complete dedicated ebook reader.

One document per window. Multiple files open multiple windows.

## What Framework is not

- **Not an editor.** No annotations, no form filling, no signatures
- **Not a converter.** No export, no save-as, no format conversion
- **Not a file manager.** No recent files, no library, no collections
- **Not a browser.** No tabs, no multi-document within a single window
- **Not an image viewer.** No JPEG, PNG, TIFF, SVG support

## Influences and borrowed techniques

Framework is a study in standing on shoulders. Every concrete pattern below names the upstream project, the file:line we studied, the Framework version it shipped in, and the source file where the borrow lives — full chain of attribution so anyone can audit the lineage. Per-technique attribution lives here; per-source-file SPDX headers stay `GPL-3.0-or-later` regardless of source.

The four projects in the opening "what this is" line — **SumatraPDF**, **zathura-pdf-mupdf** / **Zathura**, **YACReader**, and **Foliate** (with its parser library **foliate-js**) — are the explicit foundations: I picked the parts I liked most and glued them into one viewer. **Sioyek**, **Plato**, **MComix**, and **Komikku** contributed targeted patterns or design references on top of that.

### [SumatraPDF](https://www.sumatrapdfreader.org/) &mdash; *spiritual foundation, cache & threading*
Copyright © 2006–2024 the SumatraPDF project authors. Licensed [GPL-3.0](https://github.com/sumatrapdfreader/sumatrapdf/blob/master/COPYING) (compatible).

Sumatra's "just a viewer" philosophy &mdash; uncompromising performance, no editor features, no library manager, every action visible &mdash; is Framework's design north star. Specific borrows:

- **Engine abstraction layer** (`src/EngineBase.h`, `src/EngineMupdf.cpp`) &mdash; the shape of our `FwDocument` interface follows the same pattern. Conceptual reference, not a code copy.
- **Render-cache state machine** (`src/RenderCache.cpp`) &mdash; per-thread `curReqs[]` with abort cookies, semaphore-driven worker dispatch, and the "promote duplicate request to head of queue" trick informed `fw-cache.c`.
- **Bytes-aware cache cap** (v0.16; `src/fw-cache.c`) &mdash; `total_cached_bytes` tracking + 512 MB default `byte_cap` with `FW_CACHE_BYTES_CAP_MB` override, replacing the page-count window. Pattern from `RenderCache.cpp:178` (`FreeIfFull`).
- **In-flight render cancellation via `fz_cookie`** (v0.17; `src/fw-document-pdf.c:pdf_cancel_render`) &mdash; per-render `fz_cookie` published under a separate `cookies_lock` mutex so cancel never blocks on the worker; `fz_run_page` sees `cookie->abort = 1` and bails at its next checkpoint. Pattern from `EngineMupdf.cpp:3178`.
- **Auto-reload on file change** (v0.21; `src/fw-window.c`) &mdash; `GFileMonitor`-driven swap with state-restore, the LaTeX/Typst killer feature. Conceptual pattern from Sumatra's `FileWatcher.cpp` reimagined in GIO idioms.
- **Smart text selection** (v0.19; `src/fw-document-pdf.c:pdf_select_at` + `pdf_get_selection_quads`) &mdash; double-click word, triple-click line, per-line drag highlights. Pattern adapted from `TextSelection.cpp` `FindClosestGlyph` / `SelectWordAt`, implemented via MuPDF's `fz_snap_selection` + `fz_highlight_selection` (which do the same thing the canonical way).

### [zathura-pdf-mupdf](https://github.com/pwmt/zathura-pdf-mupdf) and [Zathura](https://pwmt.org/projects/zathura/) &mdash; *render pipeline & sandboxing*
Copyright © 2009–2024 pwmt.org. Licensed [Zlib](https://github.com/pwmt/zathura/blob/develop/LICENSE) (compatible).

zathura-pdf-mupdf's zero-copy `MuPDF`→`cairo` pipeline is the textbook minimalist implementation; the parent zathura project's render scheduler is the GLib-native blueprint Framework's cache mirrors.

- **Zero-copy MuPDF render** (v1.6; `src/fw-document-pdf.c:render_page_direct`) &mdash; constructs the MuPDF pixmap *around* the cairo surface buffer via `fz_new_pixmap_with_bbox_and_data` + `fz_device_bgr`, eliminating the channel-shuffle loop entirely. Pattern from `zathura-pdf-mupdf/render.c`.
- **Cached `fz_stext_page` per page** (v0.18; `src/fw-document-pdf.c:pdf_get_or_extract_stext`) &mdash; lazy per-document cache populated on first text-related call. Search across 901 pages: 332 ms cold → 48 ms warm (6.85× speedup). Pattern from `zathura-pdf-mupdf/page.c`.
- **`g_thread_pool_set_sort_function` priority dispatch** (v0.14; `src/fw-cache.c:render_job_compare`) &mdash; the pool reorders queued jobs by `last_view_time` so the most recently prioritized page runs next. Pattern from `zathura/render.c:94`.
- **Hue-preserving recolor** (v0.22; `src/fw-view.c` snapshot path) &mdash; luminance-aware affine that flips the lightness axis while preserving each pixel's chromatic offset. Red diagrams stay red on a dark background. Conceptually similar to `zathura/render.c`'s `colorumax` HSL recolor, reduced to a 4×4 GSK matrix.
- **Landlock LSM hardening** (v0.38; `src/fw-sandbox.c`) &mdash; drops filesystem `EXECUTE` + `MAKE_*` rights at startup so a malicious document exploiting MuPDF / DjVuLibre / libarchive into RCE can't escalate to spawning a shell or planting a backdoor. Pattern from `zathura/landlock.c`.

### [YACReader](https://www.yacreader.com/) &mdash; *comic-reading polish*
Copyright © Luis Ángel San Martín. Licensed [GPL-3.0](https://github.com/YACReader/yacreader/blob/master/COPYING) (compatible).

YACReader is the comic reader I keep going back to on Linux; its loupe and double-spread detection are the parts I wanted in Framework.

- **Magnifying loupe** (v0.24; `src/fw-view.c` snapshot path) &mdash; circular zoom-around-cursor viewport, pure GPU work via `gtk_snapshot_push_rounded_clip` + scale transform + texture re-append. F7 toggle. Pattern from `YACReader/magnifying_glass.cpp`.
- **Aspect-ratio double-spread detection** (v0.27.1; `src/fw-view.c::view_page_is_spread`) &mdash; pages with `w/h > 1.0` are treated as pre-rendered spreads and stand alone in facing-pages mode, with the page that would have been their natural partner orphaning so alternation resumes after.
- **Filename-based double-spread detection** (v0.37; `src/fw-document-cbr.c::cbr_is_spread_filename`) &mdash; complements the aspect-ratio test by catching scanlation rips where the spread image has portrait dimensions but the filename concatenates two consecutive page numbers (`chapter01_034035.jpg`). Direct port of YACReader's algorithm at `common/comic.cpp:925-1028`.

### [Foliate](https://johnfactotum.github.io/foliate/) and [foliate-js](https://github.com/johnfactotum/foliate-js) &mdash; *reflow architecture and format-parsing canon (Phase 13.1)*
Copyright © John Factotum. Foliate licensed [GPL-3.0-or-later](https://github.com/johnfactotum/foliate/blob/master/LICENSE) (compatible). foliate-js licensed [MIT](https://github.com/johnfactotum/foliate-js/blob/main/LICENSE) (compatible).

Foliate is the serious GNOME ebook reader; foliate-js is its parser library. The Phase 13.1 reflow rewrite is built on Foliate's architecture and ports format parsers from foliate-js into C. (Earlier patchnotes from v0.40.0 onward call this work "Fractal-style" — that was a slip; the actual reference Brandon meant was always Foliate. The architectural pattern is correct; the name was wrong.)

- **`GListModel`-of-blocks + `GtkSignalListItemFactory` + per-row widget pattern** (v0.40.0+; `src/fw-reflow-view.c`) &mdash; structurally-typed items (paragraph, heading, image, blockquote, list, code) rendered into native widgets that wrap text natively, instead of pre-rendered to pixmaps. Pattern equivalent to Foliate's reader.js row-rendering chain.
- **MOBI / KF7 / KF8 / AZW3 parser** (planned, Phase 13.1 Phase 4–5) &mdash; PalmDB envelope walk, PalmDOC LZ77 decompressor, EXTH metadata extraction, KF7 HTML stream / KF8 part-table dispatch. C port of `.foliate-js/mobi.js`.
- **EPUB OPF spine walker** (v0.42.0; `src/fw-reflow-document-epub.c`) &mdash; container.xml → OPF → manifest + spine + metadata; per-chapter XHTML through GMarkupParser. C port of `.foliate-js/epub.js`'s structural walk.
- **FB2 walker** (v0.41.0; `src/fw-reflow-document-fb2.c`) &mdash; FictionBook XML through GMarkupParser; section nesting + inline styles + `<binary>` base64 → GdkTexture. C port of `.foliate-js/fb2.js`.
- **Pagination math** (v0.48.0; `src/fw-reflow-view.c::recompute_pagination`) &mdash; viewport-driven block-level pagination. Block-only granularity for now; line-level Pango-split is a follow-up. Pattern conceptually from `.foliate-js/paginator.js`.
- **Reading-position persistence** (v0.50.0; `src/fw-state.c`) &mdash; first-block index of active page survives across sessions. Pattern from Foliate's reader.js.

### [Sioyek](https://sioyek.info/) &mdash; *zoom transitions, async search, threading hybrid*
Copyright © Ali Mostafavi. Licensed [GPL-3.0](https://github.com/ahrm/sioyek/blob/main/LICENSE) (compatible).

Sioyek's `pdf_renderer.cpp` is the most carefully tuned single-document Linux MuPDF reader I found.

- **`try_closest_rendered_page` zoom transition** (v0.28; `src/fw-cache.c::fw_cache_get_texture`) &mdash; each `CacheEntry` retains up to 3 prior-zoom snapshots; nearest-zoom slot returned for GTK to auto-scale, so continuous Ctrl+scroll zoom shows a sharp preview instead of a blurry one-zoom-back fallback. Pattern from `pdf_renderer.cpp:236`.
- **TTL+LRU hybrid eviction** (v0.26; `src/fw-cache.c`) &mdash; `last_access_us` per `CacheEntry` bumped on every hit; bytes-cap eviction sorts outside-priority candidates oldest-first. Pattern from `pdf_renderer.cpp:269` `delete_old_pages`.
- **Async, progressive search worker** (v0.7; `src/fw-search.c`) &mdash; dedicated worker thread scans page-by-page and posts hits back to the main loop via signals as they're found, with generation-counter cancellation. Pattern from `pdf_renderer.cpp::run_search`.
- **Hybrid threading model** (validated as not currently needed in v0.36; tracked as fallback) &mdash; one parent `fz_context`, per-thread `fz_clone_context`, per-(thread, path) `fz_document`. Sits between Sumatra's full-clone and Framework's 8-independent-document model on the memory/parallelism curve. Stays as a roadmap option if a memory-constrained deployment target ever shows up.

### [Plato](https://github.com/baskerville/plato) &mdash; *memory-pressure reference*
Copyright © 2017 Bastien Dejean. Licensed AGPL-3.0.

Plato runs MuPDF on Kobo e-readers (single-core ARM, ~256 MB RAM). **Technique reference only** &mdash; AGPL-3.0 is not source-compatible with our GPL-3-or-later, so no code is copied.

- **Per-instance MuPDF store size scaling** (v0.26; `src/fw-document-pdf.c:pdf_pick_store_size`) &mdash; Plato uses a 32 MB MuPDF store cap (`crates/core/src/document/mupdf_sys.rs:37`), half of what Sumatra's `FZ_STORE_DEFAULT` and Framework's biggest tier pick. The discrepancy seeded Framework's file-size-aware ladder (16/32/64/128 MB) so novels don't pay textbook overhead and textbooks aren't pinched. Sioyek (lets MuPDF pick) and Sumatra (uses default) were the other two reference points.

### [MComix](https://sourceforge.net/projects/mcomix/) &mdash; *manga RTL navigation*
Copyright © Pontus Ekberg et al. Licensed [GPL-2.0-or-later](https://sourceforge.net/projects/mcomix/) (compatible; combined work distributable under GPL-3-or-later).

- **Manga mode RTL navigation** (v0.27; `src/fw-window.c::on_key_pressed`) &mdash; F4 toggle that swaps Left/Right arrow page-nav semantics: Left advances, Right goes back, following the reading order of Japanese manga. Conceptual pattern from MComix's `event.py`. Combined with facing-pages, also flips left/right within each pair so the lower-numbered page sits on the right.

### [Komikku](https://gitlab.com/valos/Komikku/) &mdash; *manga / webtoon reader UX*
Copyright © Valéry Febvre et al. Licensed [GPL-3.0-or-later](https://gitlab.com/valos/Komikku/-/blob/main/COPYING) (compatible).

Top-tier native GNOME manga / webtoon reader. Reference for the comic-mode UX work.

- **Webtoon mode (zero-gap continuous strip)** (v0.27; `src/fw-view.c::recompute_layout`) &mdash; F5 toggle drops `PAGE_GAP` to zero so vertically-laid-out long-strip comics stitch into a seamless single canvas. Conceptual pattern from Komikku's reader pager.
- **Facing pages with cover-standalone** (v0.27; `src/fw-view.c::view_page_is_paired`) &mdash; F10 toggle pairs pages 1+2, 3+4, etc., with page 0 as the standalone cover. Mirrors how a physical book opens.
- **Phase 13.1 reader-pager pattern reference** (planned) &mdash; secondary reference for the Foliate-style EPUB reflow rewrite, alongside foliate-js itself. See `.komikku/komikku/reader/pager/`.

### Rendering engines (system libraries, no source vendored)

- **[MuPDF](https://mupdf.com/)** — PDF / XPS / EPUB / MOBI / FB2 / CBZ rendering, structured-text extraction, font/image stores. Copyright © Artifex Software. Licensed [AGPL-3.0](https://www.gnu.org/licenses/agpl-3.0.html). The shipping binary is effectively AGPL because of this link — see the [License](#license) section.
- **[DjVuLibre](http://djvu.sourceforge.net/)** — DjVu rendering. Copyright © Léon Bottou et al. Licensed [GPL-2.0-or-later](https://djvu.sourceforge.net/COPYRIGHT.html).
- **[libarchive](https://www.libarchive.org/)** — RAR / 7-Zip / tar / ZIP enumeration for the CBR backend (so we can ship CBR support without the libunrar licensing trap). Copyright © Tim Kientzle et al. Licensed [BSD-2-Clause](https://github.com/libarchive/libarchive/blob/master/COPYING).

### Platform (system libraries, no source vendored)

- **[GTK](https://www.gtk.org/)** (4.16+) and **[libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/)** (1.7+) — UI toolkit. LGPL-2.1-or-later.
- **[Cairo](https://www.cairographics.org/)** (1.18+) — image surface management, the buffer MuPDF and DjVuLibre render directly into. LGPL-2.1-or-later / MPL-1.1.
- **[GLib](https://docs.gtk.org/glib/)** (2.82+) — data structures, threading (`GThreadPool`), GObject, async I/O via GIO (file monitoring, archives, portals). LGPL-2.1-or-later.
- **[JSON-GLib](https://gnome.pages.gitlab.gnome.org/json-glib/)** (1.10+) — per-document state persistence at `$XDG_DATA_HOME/framework/state.json`. LGPL-2.1-or-later.
- **[Pango](https://docs.gtk.org/Pango/)** (via GTK) — text layout for the search bar and dialogs (and the planned Phase 13.1 reflow mode). LGPL-2.1-or-later.
- **[Linux kernel Landlock LSM](https://landlock.io/)** (kernel 5.13+, ABI 1+) — process-scoped filesystem sandboxing applied at startup. Optional at runtime; the binary degrades to a no-op on older kernels.

### Tooling

- **[Meson](https://mesonbuild.com/)** (1.4+) and **[Ninja](https://ninja-build.org/)** — build system.
- **[gettext](https://www.gnu.org/software/gettext/)** — i18n scaffolding (active translations are TBD; the infrastructure is wired).
- **[gcc](https://gcc.gnu.org/)** / **[clang](https://clang.llvm.org/)** + **[AddressSanitizer / UndefinedBehaviorSanitizer](https://github.com/google/sanitizers)** — used during development; not shipping deps.

Thanks to the [GNOME](https://www.gnome.org/) project for the platform that makes this kind of single-purpose viewer possible at all.

## License

Framework's source code is licensed under the [GNU General Public License, version 3 or later](LICENSE).

Because Framework links against [MuPDF](https://mupdf.com/) (AGPL-3.0), the **shipping binary** is effectively AGPL-3.0 &mdash; redistributors must make corresponding source available. Framework's *source* remains GPL-3-or-later: when distributing source, recipients may choose any GPL version 3 or later.

When techniques from GPL-3 sources (SumatraPDF, Sioyek, YACReader, Foliate, Komikku) are incorporated, the resulting combined work is distributable under GPL-3 (the common denominator). Zlib techniques (Zathura, zathura-pdf-mupdf), MIT techniques (foliate-js), and GPL-2-or-later techniques (MComix) are also cleanly compatible. Plato (AGPL-3.0) is technique-reference only — no code is copied. The original authors are credited in the [Influences](#influences-and-borrowed-techniques) section above and our SPDX headers stay `GPL-3.0-or-later` regardless of source.

## Support

If Framework's useful to you and you'd like to chip in:

```
bc1qkge6zr45tzqfwfmvma2ylumt6mg7wlwmhr05yv
```
