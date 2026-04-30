# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Framework is a native GNOME multi-format document viewer (PDF, DjVu, CBZ, CBR, XPS, EPUB, FB2, MOBI) — C17, GTK4/libadwaita, Meson — with a velocity-driven pre-cache engine. Project version lives in `meson.build` (currently 0.11.0 — pre-1.0; see the v0.6.0 patchnote for why we backed off the earlier 1.x line). `spec.md` is the authoritative design doc; `roadmap.md` tracks phase status; `patchnotes.md` is per-release notes.

## Build & run

Standardize on `builddir` (not `build` — see README §"Building"). Configure once, then compile:

```sh
meson setup builddir
meson compile -C builddir          # or: ninja -C builddir
./builddir/src/framework path/to/doc.pdf
./builddir/src/framework --version
```

For a single-file rebuild during iteration: `ninja -C builddir src/framework.p/<file>.c.o`. There is no test target — verification is by running the binary against real documents.

## Runtime debug tracing

Set `FW_DEBUG=1` to enable timestamped tracing across all subsystems (initialized in `main.c` via `fw_debug_init`). Use the domain macros in `src/fw-debug.h`: `FW_TRACE_DOC`, `FW_TRACE_PDF`, `FW_TRACE_DJVU`, `FW_TRACE_CACHE`, `FW_TRACE_VIEW`, `FW_TRACE_WINDOW`, `FW_TRACE_MEM`. The check is a single atomic load — zero overhead when disabled, so leave traces in.

```sh
FW_DEBUG=1 ./builddir/src/framework file.pdf
```

For crash analysis, `coredumpctl debug` and `gdb -batch -ex run -ex 'thread apply all bt' --args ./builddir/src/framework <file>` are the patterns used in this repo.

## Test corpus

Brandon's PDF and DjVu test corpus is at **`/home/bdkl/docs/Calibre Library/`** — use it as the default source for debugging, regression testing, and benchmarking work on this codebase. Mind the spaces in the path (quote it). Examples already accepted in `.claude/settings.local.json`:

```sh
FW_DEBUG=1 ./builddir/src/framework "/home/bdkl/docs/Calibre Library/Joshua Bloch/Effective Java (5)/Effective Java - Joshua Bloch.pdf"
```

Pick samples that cover the failure modes you care about: large textbooks (font/JPEG2000-heavy, stresses the 8× context model from `fw-document-pdf.c:21–57`), comics/scanned books (cheap to render but bytes-heavy under fit-width), `.djvu` files (single-mutex render path), poster-format PDFs (high-zoom memory pressure). For "find me a random sample," `find "/home/bdkl/docs/Calibre Library" -name '*.pdf' | shuf -n 10` is the pattern in use.

## Architecture

Single-document-per-window design. `g_application_open` spawns one `FwWindow` per file argument; multi-doc UX (tabs, recents) is explicitly out of scope (`spec.md` §13).

**Layered around an abstract document interface:**

- `FwDocument` (interface, `src/fw-document.h`) — vtable with `open`/`close`, `get_page_count`, `get_page_size`, `render_page`, `get_toc`, `search`, `get_text`, `get_links`, plus the page-handle API (`open_page`/`close_page`/`render_page_from_handle`) that lets the cache separate parsing from rendering, and `cancel_render` for scrubbing aborts.
- `FwDocumentPdf` (`fw-document-pdf.c`) — MuPDF backend. Despite the name, this handles **every** MuPDF-supported format: PDF, CBZ, CB7, CBT, XPS, EPUB, FB2, MOBI. The `pdf_open` path calls `fz_register_document_handlers` + `fz_open_document`, which dispatch internally by content. Reflowable formats (EPUB / FB2 / MOBI) get an `fz_layout_document(600, 900, 11)` pass per render-instance open. Type name stayed `FwDocumentPdf` to avoid a churn-only rename; treat it as "the MuPDF backend."
- `FwDocumentDjvu` (`fw-document-djvu.c`) — DjVuLibre backend.
- `FwDocumentCbr` (`fw-document-cbr.c`) — RAR/7z/tar comics via libarchive. Single-mutex per archive (libarchive isn't thread-safe per-reader). Render path: extract entry bytes → `fz_new_image_from_buffer` → `fz_fill_image` into a draw device wrapping the cairo surface buffer (zero-copy). Page sizes default to page 0's dimensions, get corrected per-page on first render.
- `fw_document_new_for_path` is the factory; backend is chosen by extension. PDF + ZIP-comic archives + XPS + reflowable formats → MuPDF backend; `.djvu`/`.djv` → DjVu backend; `.cbr` → libarchive backend.

**The Velocity-Driven Cache (`fw-cache.c`) is the core performance differentiator** — read it before changing anything in the render path. Three tiers and a state machine:

- **Tier 0 — Thumbnails:** persistent 150px-wide previews, lazy-rendered on a separate low-priority `GThreadPool`, never evicted. Always-available placeholders during fast scroll.
- **Tier 1 — Parsed handles:** lightweight `fz_page`/`ddjvu_page` objects (~50-page window), no pixels. Eliminates I/O on render.
- **Tier 2 — Rendered surfaces:** `cairo_surface_t` + cached `GdkTexture` keyed by page. Strict eviction.
- **Render states** driven by `dy/dt` from `gtk_widget_add_tick_callback`: `STATIC` (render visible + small lookahead, idle the pool), `CRUISING` (drip-feed forward, drop backward), `SCRUBBING` (abort queue via `cancel_gen` bump, paint placeholders). View invalidation (zoom/rotation/scale) bumps `render_gen` so stale jobs become no-ops.

**Threading rules — load-bearing, do not violate:**

- **MuPDF** is NOT thread-safe per-document. The PDF backend opens the file `MAX_RENDER_INSTANCES` (8) times, each with its own `fz_context` + `fz_document` + per-instance mutex; render threads round-robin across them. Cloned contexts share font/image stores but `fz_page`/`fz_image` lazy-read from streams owned by the document — concurrent reads on a shared document corrupt state even via display lists. Don't "simplify" this back to a single document.
- **MuPDF exception handling uses `setjmp`/`longjmp`** via `fz_try`/`fz_catch`. NEVER `return`/`goto`/`longjmp` from inside those blocks. Variables modified in `fz_try` and read in `fz_catch` must be `volatile`. Use `fz_always` for cleanup.
- **DjVuLibre** requires serialized access — single worker / mutex. The async-decode + abort queue exists specifically to keep `ddjvuapi` from CPU-locking under high-velocity scrubbing.
- The cache uses `cancel_gen` (abort scope) and `render_gen` (param-change scope) — both are `guint` counters checked inside worker jobs, not pthread cancellation. Preserve this pattern.

**View pipeline (`fw-view.c`, custom `GtkWidget`):** determines visible pages from scroll position, asks `FwCache` for surfaces, paints via `gtk_snapshot_append_texture` (cache hit) or grey placeholder + thumbnail (miss). Search highlights and selection rectangles are overlay layers, not re-renders. Wayland fractional scaling: render resolution is multiplied by widget scale factor (`fw_cache_set_scale_factor`) — don't paint upscaled bitmaps.

**Other modules:** `fw-application.c` (single-instance `AdwApplication`, file-open dispatch), `fw-window.c` (header bar, actions, keybindings, search-bar UI, navigation history — two `GArray<NavEntry>` stacks pushed only on explicit jumps: TOC click, page-entry edit, internal link click; print operation; embedded-file extraction with sanitized output paths), `fw-sidebar.c` (`GtkListView` + `GtkTreeListModel` + `FwTocItem` GObject; `fw_sidebar_set_current_page` walks the underlying `FwTocItem` tree, expands ancestor `GtkTreeListRow`s, then selects the row in the flat model), `fw-search.c` (async find controller — runs the page-by-page scan on a worker thread, posts hits back via `g_idle_add_full`, emits `hits-changed` / `current-changed` / `search-finished` signals; the view subscribes to repaint highlights and reveal the active hit), `fw-state.c` (per-document JSON state in `$XDG_DATA_HOME/framework/state.json`, LRU-pruned).

**`FwView` signals:** `page-jumped(int dest_page)` fires only on explicit navigation (currently just internal link clicks). Plain scroll, search-hit reveal, and `fw_view_go_to_page` from the window do *not* emit it. The window subscribes to push the previous viewport onto its history stack.

## Conventions

- **GObject prefix `Fw`**, function prefix `fw_`, file prefix `fw-`, GType macros `FW_TYPE_*`. New source files go in `src/` and must be added to the `framework_sources` list in `src/meson.build` — there is no glob.
- C17, `warning_level=2`. Match existing style (4-space C indent in this tree, GNU-flavored function-name-on-its-own-line for definitions). No new dependencies without asking — the dep set in `src/meson.build` is deliberate.
- **MuPDF pkg-config is broken** (often emits an empty `-I`). The build uses `cc.find_library('mupdf')` + optional `mupdf-third` directly via `declare_dependency`. Don't switch to `dependency('mupdf')`.
- App ID is `com.github.vrnvctss.framework`; the GSettings schema, desktop file, and metainfo all live under that ID in `data/`. Schema must be installed/compiled (`gnome.compile_schemas`) before settings reads work — `meson compile` handles this in-tree.
- This project is GPL-3.0-or-later. Every source file carries an `SPDX-License-Identifier` header — keep it on new files.

## Scope discipline

Framework is strictly a viewer (spec §13). Don't add: annotations, export/save-as, recent-files UI, tabs, additional formats beyond PDF/DjVu, vim bindings, modal interfaces. Phase status in `roadmap.md` is the source of truth for what's landed vs. deferred — check it before assuming a feature is missing on purpose.

## Reference repos (`.sumatrapdf/`, `.zathura/`, `.zathura-mupdf/`, `.sioyek/`, `.plato/`)

Five vendored upstream sources kept locally for technique-borrowing. All are gitignored, are not part of the Framework build, and must never be modified — treat them as read-only canon. Concrete borrow targets and ROI ranking live in `roadmap.md` Phase 11; this section is the *map of where to look*.

All cloned shallow (`--depth 1`); refresh with `git -C <dir> pull --depth 1`. The zathura mirror is `https://github.com/pwmt/zathura-pdf-mupdf.git` (the canonical `git.pwmt.org` host doesn't always resolve).

| Repo | Stack | Why it's here |
|---|---|---|
| `.sumatrapdf/` | C++ / Win32 / GDI | Most architecturally similar engine + cache layer; bundles full MuPDF source under `mupdf/`. |
| `.zathura/` | C / GTK3 / GLib | Closest peer technically — same `GThreadPool` + GObject + cairo idioms. Render scheduler is in `zathura/render.c`. |
| `.zathura-mupdf/` | C / Zlib | Tiny PDF plugin (1.3 KLOC). Source of the zero-copy MuPDF→cairo pipeline. |
| `.sioyek/` | Qt5 / C++ / OpenGL | Mature single-developer reader, MuPDF-backed, performance-tuned. Strong ideas around zoom transitions, slicing, async search. |
| `.plato/` | Rust / e-ink | Tiny memory budget. Useful pressure test for "what if RAM is *really* scarce." |
| `.sumatrapdf/mupdf/platform/gl/` | C / OpenGL | Official MuPDF reference viewer. Use to grep the canonical fitz/mupdf API patterns. |

### `.zathura-mupdf/` — minimal MuPDF→cairo blueprint

Tiny C plugin (~1.3 KLOC across `zathura-pdf-mupdf/*.c`), Zlib-licensed. This is the closest analogue to a single `FwDocumentPdf` and the source of Framework's zero-copy render technique. Read it when touching the PDF backend.

| File | What's in it |
|---|---|
| `plugin.h` | `mupdf_document_t` (single `fz_context` + `fz_document` + `GMutex`) and `mupdf_page_t` (`fz_page` + cached `fz_stext_page`). Mirrors how Framework holds backend state. |
| `render.c` | **The zero-copy pipeline.** `pdf_page_render_to_buffer` calls `fz_device_bgr` + `fz_new_pixmap_with_bbox_and_data` to wrap the cairo surface's own pixel buffer as the MuPDF render target — no intermediate pixmap, no channel shuffle. Routes through `fz_new_display_list` → `fz_run_page` → `fz_run_display_list` so the page can be parsed once and replayed. This is exactly the v1.6 technique referenced in `roadmap.md` Phase 2. |
| `document.c` | `fz_register_document_handlers`, optional epub.css load, password auth, `fz_count_pages`. Pattern for `fw_document_pdf_open`. |
| `page.c` | Page lifecycle under a single doc-level mutex; pre-extracts an `fz_stext_page` at load time and reuses it for search + selection. |
| `search.c` | `fz_search_stext_page` against the cached stext page (512-hit bound) → quads → page-coord rectangles. |
| `select.c` | `fz_copy_selection` (text) and `fz_highlight_selection` (overlay quads) on the same cached stext page. Note the `_WIN32`-vs-Unix newline flag — keep the Unix branch. |
| `links.c`, `index.c`, `attachment.c`, `image.c` | Per-feature MuPDF API recipes. |

**Threading model: a single shared `fz_document` behind one mutex.** This is *not* what Framework does — Framework's PDF backend opens the file `MAX_RENDER_INSTANCES` (8) times for true parallelism (see `fw-document-pdf.c`). Use zathura as the canonical reference for *which MuPDF calls* to make and *how to handle exceptions cleanly*; do not regress to its concurrency model.

### `.sumatrapdf/` — large-scale reference for cache, threading, and engine abstraction

Windows-native C++ application (270 MB shallow, GPL-3, bundled MuPDF + DjVuLibre + ~25 other deps under `ext/`). Most of it is Win32-specific (`HDC`, `CRITICAL_SECTION`, `HBITMAP`, GDI) and not portable, but the architecture is the spiritual model for Framework's cache and engine layer. Skim files of interest; do not try to read top-to-bottom.

**Files worth knowing:**

| File | Why it matters |
|---|---|
| `src/EngineBase.h`, `src/EngineMupdf.h/.cpp` (~4.6 KLOC), `src/EngineDjVu.cpp` (~1.4 KLOC), `src/EngineCreate.cpp` | The engine-abstraction pattern. `EngineBase` is the C++ analogue of `FwDocument`; `EngineMupdf` and `EngineDjVu` parallel `fw-document-pdf.c` and `fw-document-djvu.c`. |
| `src/RenderCache.h/.cpp` (~1 KLOC) | The render-cache state machine. Tile-based (`TilePosition` res/row/col), bounded request queue (`MAX_PAGE_REQUESTS = 8`), bounded bitmap cache (`MAX_BITMAPS_CACHED = 128`), per-thread `curReqs` + `abortCookie` for in-flight cancellation, lazy thread spawn with idle counter, semaphore-driven dispatch, `FreeNotVisible` / `Invalidate` eviction, "promote duplicate request to head of queue" trick, `ReduceTileSize` graceful degradation. The closest thing to `fw-cache.c` in any reference repo. |
| `src/DisplayModel.h/.cpp` (~2.1 KLOC) | Layout + scroll model. `RecalcVisibleParts`, `PageVisibleNearby` (the "fuzz" predicate the cache uses to decide what to keep), zoom virtuals (`kZoomFitPage`, `kZoomFitWidth`). Pattern for what `fw-view.c` already does. |
| `src/Canvas.cpp` (~3.4 KLOC) | Win32 paint loop, scroll/zoom event handling, kinetic scroll. Reference only — Framework's GTK4 widget pipeline is fundamentally different. |
| `src/EngineMupdf.cpp:1700–1855` | **MuPDF threading via `fz_locks_context` + `fz_clone_context`.** Single shared `fz_context` with locking callbacks; each render thread gets a per-thread cloned context via `GetOrClonePerThreadContext`. This is MuPDF's documented multi-threading approach and a third option besides "single mutex" (zathura) and "N independent documents" (Framework). If we ever reconsider the 8-instance approach, this is the prior art. |
| `src/PdfCreator.cpp`, `src/PdfSync.cpp` | Out of scope (creation, SyncTeX). Skip. |
| `mupdf/`, `ext/` | Bundled MuPDF source + dep tree. Useful for grepping MuPDF internals (`mupdf/include/`, `mupdf/source/`) without leaving the repo, since Fedora's `mupdf-devel` only ships headers. |

**Don't copy code verbatim.** Sumatra is GPL-3; Framework is GPL-3-or-later, so license-compatible, but it is C++/Win32 and the patterns must be re-expressed in C/GLib/GTK. Sumatra and zathura are sources of *technique*, not source code.

### `.zathura/` — render scheduling in idiomatic GLib

The `zathura-pdf-mupdf` plugin (separate `.zathura-mupdf/`) handles MuPDF integration; the *cache and dispatch* logic lives in zathura proper. The whole core is ~30 KLOC; only `zathura/render.c` (~1.1 KLOC) is directly relevant.

| File | Why it matters |
|---|---|
| `zathura/render.c` | The full GLib-native render scheduler. Reading the whole file once is worth it — it's smaller than `fw-cache.c` and shows what an idiomatic GLib alternative looks like. Highlights: a single-thread `GThreadPool` (line 92) with **`g_thread_pool_set_sort_function(render_thread_sort, NULL)`** (line 94), so pending jobs are reordered by `last_view_time` instead of being dispatched FIFO; `atomic_bool aborted` flag per job (line 79); LRU page-cache invalidation by `last_view_time` (line 977); `update_view_time` called on every paint (line 425) so the freshest viewport always wins. Hue-preserving recolor pipeline (lines ~280–340 + `colorumax`) is the source for an upgraded "Invert Colors" mode. |
| `zathura/page-widget.c` | Per-page GtkWidget that owns its own `ZathuraRenderRequest` and emits view-time updates on draw. Different organizing principle than `fw-view.c`'s single widget paints all pages, but the request-per-page idiom maps cleanly onto our `priority_order`. |
| `zathura/document.c`, `zathura/page.c` | Thin wrappers — open/close, page metadata. Skim only. |

### `.sioyek/` — Qt5 reader with strong zoom/cache patterns

PhD-research-flavored reader (~7.5 KLOC just in `pdf_renderer.cpp` + the two doc files). Most of it is Qt-specific noise; the rendering and cache logic in `pdf_viewer/pdf_renderer.cpp` is gold.

| File | Why it matters |
|---|---|
| `pdf_viewer/pdf_renderer.cpp` | (~750 lines.) **`fz_clone_context` per render thread** (line 42 `init_context`) + `(thread_index, path) → fz_document` map (line 454 `get_document_with_path`) — a third concurrency model: each thread shares one parent context but has its own per-thread document handle. Memory cost between Sumatra's full-clone and Framework's 8-instance. **`try_closest_rendered_page`** (line 236) — when the requested zoom isn't available, return the nearest cached zoom and let the GPU rescale. **Slicing** via `(slice_index, num_h_slices, num_v_slices)` on each request — the high-zoom-large-page solution Framework lacks. **Time-based cache eviction** in `delete_old_pages` (line 269): keep N most-recent always, drop anything older than `CACHE_INVALID_MILIES` via a 1Hz QTimer. **Search worker** in `run_search` (line 342): dedicated thread, `fz_new_stext_page_from_page_number` per page, emits progress every 16 pages — the model for fixing roadmap Phase 5. |
| `pdf_viewer/document.cpp` | (~4.5 KLOC.) Most is Sioyek-specific (highlights, bookmarks, annotations) — out of scope for Framework. Skim only when investigating a specific feature. |
| `pdf_viewer/document_view.cpp` | View-layer behaviors. Reference for zoom-anchor maths and continuous-mode geometry, but the Qt event model doesn't translate. |

### `.plato/` — Rust e-ink reader, the "what if RAM is scarce" reference

Built for Kobo e-readers (single-core ARM, ~256 MB RAM). The MuPDF wrapper is in `crates/core/src/document/`. Useful as a sanity check on memory-pressure decisions, not as an architectural model.

| File | Why it matters |
|---|---|
| `crates/core/src/document/pdf.rs` | (~550 lines.) Idiomatic Rust binding. Single shared `Rc<PdfContext>` — no parallelism, no cloning. Pattern for "minimum viable MuPDF wrapper." |
| `crates/core/src/document/mupdf_sys.rs` | (~340 lines.) Declares `CACHE_SIZE = 32 * 1024 * 1024` (line 37) — Plato runs MuPDF with **half** the store size Framework uses. If a textbook OOMs on a memory-constrained device, this is the knob. |

### License compatibility matrix

Framework is **GPL-3.0-or-later**. All borrowed-code attributions go in `README.md` "Influences and borrowed techniques"; the SPDX identifier on every Framework source file stays `GPL-3.0-or-later` regardless of source.

| Source | License | Code copy allowed? | Notes |
|---|---|---|---|
| zathura | Zlib | yes | Permissive. Must not misrepresent origin (Zlib §1); attribution preserves required notice. |
| zathura-pdf-mupdf | Zlib | yes | Same. |
| SumatraPDF | GPL-3.0 (source headers say `License: GPLv3`; readme's "(A)GPLv3" wording reflects that the *binary* link with AGPL'd MuPDF is effectively AGPL — the source itself is GPL-3) | yes | Combined work distributable under GPL-3 (the common denominator with GPL-3-or-later). |
| Sioyek | GPL-3.0 | yes | Same as Sumatra. |
| Plato | **AGPL-3.0** | **NO source copies** | Technique reference only. Copying code would force Framework to AGPL. |
| MuPDF (system dep, also bundled in `.sumatrapdf/mupdf/`) | AGPL-3.0 | **NO source copies** | We link the system library — fine. The shipping binary is effectively AGPL because of this link, even though Framework's source stays GPL-3-or-later. |
| DjVuLibre (system dep) | GPL-2-or-later | linking only | Already credited in README dependency table. |

**Implications for distributors:**
- Framework source: GPL-3-or-later (recipient can choose GPL-3 or any later GPL).
- Framework binary as shipped: effectively AGPL-3 due to MuPDF link (corresponding source must be made available).
- If you cherry-pick GPL-3 code from Sumatra or Sioyek into Framework, that *file* should still carry our SPDX header — the original copyright notice goes in `README.md` attribution, not duplicated per-file.

**Do not** copy code from Plato or from `.sumatrapdf/mupdf/source/`. Pattern-borrow only.

### `.sumatrapdf/mupdf/platform/gl/` — official MuPDF reference

Bundled inside Sumatra's tree. Use as the canonical answer to "what's the right way to call this fitz API?" — the MuPDF authors wrote it. Not architecturally interesting (single-threaded, custom in-tree UI toolkit), but `gl-main.c:1052/1067` shows the textbook `fz_new_draw_device` setup and `gl-main.c:3309` shows context creation patterns. Grep here before guessing about `fz_*` semantics.
