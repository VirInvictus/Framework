# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Framework is a native GNOME multi-format document viewer (PDF, DjVu, CBZ, CB7, CBT, CBR, XPS, EPUB, FB2, MOBI, AZW3) — C17, GTK4/libadwaita, Meson. It has **two render pipelines**: a velocity-driven pre-cache engine for fixed-layout formats (PDF/DjVu/comics/XPS), and a native-GTK reflow stack for ebook formats (EPUB/FB2/MOBI/AZW3/TXT) that bypasses MuPDF's fixed layout. Project version lives in `meson.build` (currently 0.65.0 — pre-1.0; see the v0.6.0 patchnote for why we backed off the earlier 1.x line). `spec.md` is the authoritative design doc; `roadmap.md` tracks phase status; `patchnotes.md` is per-release notes.

## Build & run

Standardize on `builddir` (not `build` — see README §"Building"). Configure once, then compile:

```sh
meson setup builddir
meson compile -C builddir          # or: ninja -C builddir
./builddir/src/framework path/to/doc.pdf
./builddir/src/framework --version
```

For a single-file rebuild during iteration: `ninja -C builddir src/framework.p/<file>.c.o`. The default build has no test target; the stress/bench suite is gated behind `-Dstress=true` (see below). Day-to-day verification is by running the binary against real documents.

**GSettings in dev runs.** As of v0.14.0 the binary calls `g_settings_new(APP_ID)`, which looks up the schema from the system schema dir by default. The dev build's compiled schema lives at `builddir/data/gschemas.compiled`. Either set `GSETTINGS_SCHEMA_DIR=builddir/data` per invocation, or use `meson devenv -C builddir` to open a subshell where the right env vars are set. Without one of these the binary aborts on launch with `Settings schema 'io.github.virinvictus.framework' is not installed`. The Flatpak build is unaffected — the schema gets installed and compiled into `/app/share/glib-2.0/schemas/`.

**Stress harness and sanitizers (v0.15+).** The `tests/` tree is gated by `-Dstress=true` and built off by default. To enable:

```sh
meson setup builddir -Dstress=true
meson compile -C builddir
GSETTINGS_SCHEMA_DIR=builddir/data ./builddir/tests/stress/stress-scrub <pdf>
```

Sanitizer builds use the `-Dsanitize=` array option (choices: `address`, `undefined`, `leak`, `thread`). `address` and `undefined` are the standard dev combo and are confirmed clean across all three stress tests; `liblsan` and `libtsan` are not installed on Brandon's Fedora and would need `sudo dnf install` first if you want them.

```sh
meson configure builddir -Dsanitize=address,undefined
meson compile -C builddir
GSETTINGS_SCHEMA_DIR=builddir/data \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ./builddir/tests/stress/stress-scrub <pdf>
# revert: meson configure builddir -Dsanitize=
```

The src layer is now a `framework-core` static library plus a thin `framework` executable. Tests link against `framework_lib_dep` to reach internal symbols. `meson test -C builddir` runs registered targets — the suite is `stress-scrub`, `stress-zoom-storm`, `stress-search-cache`, `stress-multidoc`, `stress-corpus-soak` (60–300 s timeouts; default corpus = `/home/bdkl/docs/Calibre Library/`). `bench-render` is built but not registered (latency benchmark, not pass/fail) — invoke directly with `--pages` / `--stride` / `--zoom`.

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

Pick samples that cover the failure modes you care about: large textbooks (font/JPEG2000-heavy, stresses the 8× context model from `fw-document-pdf.c` — see `MAX_RENDER_INSTANCES` and the comment block above it), comics/scanned books (cheap to render but bytes-heavy under fit-width), `.djvu` files (single-mutex render path), poster-format PDFs (high-zoom memory pressure). For "find me a random sample," `find "/home/bdkl/docs/Calibre Library" -name '*.pdf' | shuf -n 10` is the pattern in use.

## Architecture

Single-document-per-window design. `g_application_open` spawns one `FwWindow` per file argument; multi-doc UX (tabs, recents) is explicitly out of scope (`spec.md` §13).

**Layered around an abstract document interface:**

- `FwDocument` (interface, `src/fw-document.h`) — vtable with `open`/`close`, `get_page_count`, `get_page_size`, `render_page`, `get_toc`, `search`, `get_text`, `get_links`, the page-handle API (`open_page`/`close_page`/`render_page_from_handle`) that lets the cache separate parsing from rendering, `cancel_render` for scrubbing aborts, `get_attachments`/`save_attachment` (PDF /EmbeddedFiles), `get_metadata` (Document Properties dialog), `select_at` + `get_selection_quads` (smart text selection — backed by the per-page cached `fz_stext_page`).
- `FwDocumentPdf` (`fw-document-pdf.c`) — MuPDF backend. Despite the name, this handles **every** MuPDF-supported format: PDF, CBZ, CB7, CBT, XPS, and (as a fallback) EPUB, FB2, MOBI, AZW3. **Comic DPI normalization (v0.70):** MuPDF's image document sizes comic pages by each image's embedded DPI, so a CBZ with inconsistent DPI metadata (some scanlations) would report same-pixel pages at very different point sizes and render them at different scales. `pdf_open` detects comic formats (CBZ/CB7/CBT by extension) and normalizes every page to the median page height (aspect ratio is DPI-invariant), storing a per-page `norm_scale` that `pdf_render_page` folds into the render zoom so the texture matches the normalized layout size. PDFs/XPS are untouched. (`fw-view.c` also carries a fit-width variance guard as a backstop for any residual size noise.) The `pdf_open` path calls `fz_register_document_handlers` + `fz_open_document`, which dispatch internally by content. **Note (v0.40+):** the ebook formats (EPUB/FB2/MOBI/AZW3) no longer reach this backend by default — the window routes them to the native reflow pipeline first (see below) and only falls back to MuPDF when reflow open fails. The reflowable-format code path here (`fz_layout_document(600, 900, 11)` per render-instance open) is therefore now the fallback, not the default. Type name stayed `FwDocumentPdf` to avoid a churn-only rename; treat it as "the MuPDF backend." Carries two backend-internal caches: `active_cookies[]` for in-flight `fz_cookie` cancellation (Phase 11 Tier 1, v0.17), and `stext_cache` for per-page structured text reused by selection and search, LRU-evicted to a cap (default 512 pages, `FW_STEXT_CACHE_CAP` override) since v0.65 (Phase 11 Tier 1, v0.18).
- `FwDocumentDjvu` (`fw-document-djvu.c`) — DjVuLibre backend.
- `FwDocumentCbr` (`fw-document-cbr.c`) — `.cbr` (RAR) comics via libarchive. (ZIP/7z/tar comics go to the MuPDF backend for its fast random page access; RAR has no central directory, so it needs this sequential-walk backend.) Single-mutex per archive (libarchive isn't thread-safe per-reader). Render path: extract entry bytes → `fz_new_image_from_buffer` → `fz_fill_image` into a draw device wrapping the cairo surface buffer (zero-copy). Page sizes are **pixel** dimensions (`img->w/h`), so DPI metadata can't distort scale. Sizes default to page 0's dimensions at open and are corrected two ways: per-page on first render, and by a one-shot background dimension probe (v0.69) that decompresses the archive once, reads real per-page pixel sizes, and emits `FwDocument::geometry-changed` so fit-width can normalize off the typical body page.
- `fw_document_new_for_path` (`fw-document.c`) is the **fixed-layout** factory; backend is chosen by extension. PDF + ZIP-comic archives (CBZ/CB7/CBT) + XPS + (fallback) reflowable formats → MuPDF backend; `.djvu`/`.djv` → DjVu backend; `.cbr` → libarchive backend.
- **Open dispatch lives in `fw-window.c::fw_window_open_file`, not the factory.** For each path it first calls `fw_reflow_path_is_supported` (TXT/FB2/EPUB/MOBI/PRC/AZW/AZW3); if true it tries `fw_window_open_reflow` (native reflow pipeline) and only on failure falls through to `fw_document_new_for_path` (MuPDF). So the *effective* default for ebook formats is reflow; the MuPDF fixed-layout path is the automatic fallback. The window holds either a `document` + `FwView` **or** a `reflow_doc` + `FwReflowView`, swapped via a `GtkStack` with `"empty" | "document" | "reflow"` pages.

**The Native Reflow Pipeline (`fw-reflow-*.c`)** is the second render path — ebook formats rendered as native GTK widgets, not rasterized pages. Foliate-style: a `GListModel` of structurally-typed blocks drives a `GtkListView` with one wrapping widget per block. See `docs/foliate-rewrite.md` for the design and Phase 13.1 in `roadmap.md` for status (Phase 1–5 shipped; Phase 6 polish open).

- `FwReflowDocument` (interface, `src/fw-reflow-document.h`) — vtable with `open`/`close`, `get_block_model` (hot path, bound directly to the `GtkListView`), `get_image`, `get_toc`, `find_block_by_anchor`, `get_metadata`. `FwBlock` is the structurally-typed item (paragraph/heading/image/blockquote/code/list-item/HR/chapter-marker, with `flags` for cover/indent/dropcap/caption and `level` for ordered-list numbering). `fw_reflow_document_new_for_path` is the reflow factory; `fw_reflow_path_is_supported` is the gate the window checks. `fw_reflow_document_search` (v0.66) is a non-vtable wrapper: a generic synchronous scan over the shared block model (visible text extracted by stripping tags + decoding entities, codepoint-wise case-insensitive substring match) returning `GArray<FwReflowHit>` — there's no per-backend search override.
- Reflow backends: `FwReflowDocumentTxt` (`fw-reflow-document-txt.c`, v0.40), `FwReflowDocumentFb2` (`fw-reflow-document-fb2.c`, libxml2 walker since v0.57), `FwReflowDocumentEpub` (`fw-reflow-document-epub.c`, libarchive ZIP → OPF/spine → `htmlReadMemory` per chapter, NCX + EPUB3 nav.xhtml TOC, since v0.42/v0.56), `FwReflowDocumentMobi` (`fw-reflow-document-mobi.c`, KF7 + KF8/AZW3, v0.52/v0.55). The low-level PalmDB/PalmDOC/HuffDic/EXTH/KF8 byte-format parsing lives in `fw-mobi-parser.c` (a standalone parser, ported line-by-line from `.foliate-js/mobi.js`).
- `FwReflowView` (`fw-reflow-view.c`, the reflow analogue of `FwView`) — hosts the `GtkListView` + `GtkSignalListItemFactory`, owns block-level pagination (`recompute_pagination`), per-block typography (justification, first-line indent, bullets/numbers, captions, raised caps), font-size adjustment, anchor scrolling, and search-hit highlighting (v0.66: splices Pango `<span background>` into block markup at bind time, breaking the span around inline tags to keep nesting valid; the window drives it via `fw_reflow_view_set_search_hits`/`set_active_hit`/`clear_search`). `FwReflowSidebar` (`fw-reflow-sidebar.c`) is the reflow TOC sidebar.

**The Velocity-Driven Cache (`fw-cache.c`) is the core performance differentiator** — read it before changing anything in the render path. Three tiers and a state machine:

- **Tier 0 — Thumbnails:** persistent 150px-wide previews, lazy-rendered on a separate low-priority `GThreadPool`, never evicted. Always-available placeholders during fast scroll.
- **Tier 1 — Parsed handles:** lightweight `fz_page`/`ddjvu_page` objects, populated lazily by render workers, evicted with the priority window. No pixels. Eliminates I/O on render.
- **Tier 2 — Rendered surfaces:** `cairo_surface_t` + cached `GdkTexture` keyed by page. **Bytes-aware cap (v0.16)**: `total_cached_bytes` is tracked live; eviction fires only when over `byte_cap` (default 512 MB, override via `FW_CACHE_BYTES_CAP_MB`). Outside-priority pages stay cached for fast scroll-back when there's headroom; visible/priority pages are never evicted.
- **Render states** driven by `dy/dt` from `gtk_widget_add_tick_callback`: `STATIC` and `CRUISING` use a symmetric ±10 priority window (v0.14); `SCRUBBING` aborts the queue via `cancel_gen` bump and triggers `fz_cookie`-based mid-render abort on PDF (v0.17), painting thumbnail placeholders. View invalidation (zoom/rotation/scale) bumps `render_gen` so stale jobs become no-ops.
- **Pool dispatch (v0.14):** `g_thread_pool_set_sort_function` reorders queued jobs by `last_view_time` so the most-recently-prioritized page runs next, regardless of when its job was pushed. Newly-visible pages skip ahead of stale queued jobs.

**Threading rules — load-bearing, do not violate:**

- **MuPDF** is NOT thread-safe per-document. The PDF backend opens the file `MAX_RENDER_INSTANCES` (8) times, each with its own `fz_context` + `fz_document` + per-instance mutex; render threads round-robin across them. Cloned contexts share font/image stores but `fz_page`/`fz_image` lazy-read from streams owned by the document — concurrent reads on a shared document corrupt state even via display lists. Don't "simplify" this back to a single document.
- **MuPDF exception handling uses `setjmp`/`longjmp`** via `fz_try`/`fz_catch`. NEVER `return`/`goto`/`longjmp` from inside those blocks. Variables modified in `fz_try` and read in `fz_catch` must be `volatile`. Use `fz_always` for cleanup.
- **DjVuLibre** requires serialized access — single worker / mutex. The async-decode + abort queue exists specifically to keep `ddjvuapi` from CPU-locking under high-velocity scrubbing.
- The cache uses `cancel_gen` (abort scope) and `render_gen` (param-change scope) — both are `guint` counters checked inside worker jobs, not pthread cancellation. Preserve this pattern.

**View pipeline (`fw-view.c`, custom `GtkWidget`):** determines visible pages from scroll position, asks `FwCache` for surfaces, paints via `gtk_snapshot_append_texture` (cache hit) or grey placeholder + thumbnail (miss). Search highlights and selection rectangles are overlay layers, not re-renders. Wayland fractional scaling: render resolution is multiplied by widget scale factor (`fw_cache_set_scale_factor`) — don't paint upscaled bitmaps.

**Other modules:** `fw-application.c` (single-instance `AdwApplication`, file-open dispatch), `fw-window.c` (header bar, actions, keybindings, search-bar UI, navigation history with two `GArray<NavEntry>` stacks pushed only on explicit jumps; print operation; embedded-file extraction with sanitized output paths; Document Properties + Keyboard Shortcuts dialogs; `AdwToastOverlay` wrapping the content tree; `GFileMonitor`-backed auto-reload that saves state, re-opens the document, and toasts on every `CHANGES_DONE_HINT`; the reflow-vs-fixed open dispatch above; comic-layout toggles — manga/webtoon/facing-pages), `fw-sidebar.c` (`GtkListView` + `GtkTreeListModel` + `FwTocItem` GObject; `fw_sidebar_set_current_page` walks the underlying `FwTocItem` tree, expands ancestor `GtkTreeListRow`s, then selects the row in the flat model), `fw-search.c` (async find controller — runs the page-by-page scan on a worker thread, posts hits back via `g_idle_add_full`, emits `hits-changed` / `current-changed` / `search-finished` signals; the view subscribes to repaint highlights and reveal the active hit; detached-worker model since v0.65 so cancel never blocks the main loop), `fw-state.c` (per-document JSON state in `$XDG_DATA_HOME/framework/state.json`, LRU-pruned to 500 entries; stores page/scroll/zoom/rotation for fixed-layout and first-block index for reflow), `fw-sandbox.c` (Landlock LSM hardening — `fw_sandbox_drop_execute` drops filesystem EXECUTE + mknod rights at startup so a document-parser RCE can't escalate; no-op on old kernels; v0.38), `fw-fonts.c` (`fw_fonts_register` registers bundled font dirs with FontConfig so reflow text renders without depending on system fonts), `fw-debug.c` (the `FW_DEBUG` trace machinery).

**`FwView` signals:** `page-jumped(int dest_page)` fires only on explicit navigation (currently just internal link clicks). Plain scroll, search-hit reveal, and `fw_view_go_to_page` from the window do *not* emit it. The window subscribes to push the previous viewport onto its history stack.

## Conventions

- **GObject prefix `Fw`**, function prefix `fw_`, file prefix `fw-`, GType macros `FW_TYPE_*`. New source files go in `src/` and must be added to the `framework_sources` list in `src/meson.build` — there is no glob.
- C17, `warning_level=2`. Match existing style (4-space C indent in this tree, GNU-flavored function-name-on-its-own-line for definitions). No new dependencies without asking — the dep set in `src/meson.build` is deliberate.
- **MuPDF pkg-config is broken** (often emits an empty `-I`). The build uses `cc.find_library('mupdf')` + optional `mupdf-third` directly via `declare_dependency`. Don't switch to `dependency('mupdf')`.
- App ID is `io.github.virinvictus.framework`; the GSettings schema, desktop file, and metainfo all live under that ID in `data/`. Schema must be installed/compiled (`gnome.compile_schemas`) before settings reads work — `meson compile` handles this in-tree.
- This project is GPL-3.0-or-later. Every source file carries an `SPDX-License-Identifier` header — keep it on new files.

## Scope discipline

Framework is strictly a viewer (spec §13). Don't add: annotations, export/save-as, recent-files UI, tabs, formats beyond the shipped set (PDF/DjVu/CBZ/CB7/CBT/CBR/XPS/EPUB/FB2/MOBI/AZW3), vim bindings, modal interfaces. Phase status in `roadmap.md` is the source of truth for what's landed vs. deferred — check it before assuming a feature is missing on purpose.

## Reference repos (`.foliate/`, `.foliate-js/`, `.komikku/`)

Three vendored upstream sources kept locally for the Phase 13.1
reflow work (Phase 1–5 shipped; Phase 6 polish open — see
`docs/foliate-rewrite.md`). All are gitignored, not part of the
build, must never be modified — read-only canon.

The other seven reference repos that historically lived here
(zathura, zathura-mupdf, sumatrapdf, sioyek, plato, yacreader,
mcomix) were removed in v0.39.0 after their patterns were extracted
into Framework — see `roadmap.md` and the v0.39.0 patchnote for the
complete list of borrows. The "Influences and borrowed techniques"
section in `README.md` retains the per-pattern attribution.

All three retained repos are cloned shallow (`--depth 1`); refresh
with `git -C <dir> pull --depth 1`.

| Repo | Stack | Why it's here |
|---|---|---|
| `.foliate/` | JavaScript / GJS | Native GNOME ebook reader. Reference for the overall reader UX, pagination cadence, font preferences and reading-position model. |
| `.foliate-js/` | JavaScript (browser) | Foliate's parser library. **The canonical implementation reference for EPUB / MOBI / AZW3 / FB2 / PDB-based formats.** Specifically `.foliate-js/mobi.js` for the PalmDB / PalmDOC / KF7 / KF8 chain, `.foliate-js/epub.js` for OPF + spine + NCX walking, `.foliate-js/fb2.js` for the FictionBook XML walker, `.foliate-js/paginator.js` for pagination math. MIT-licensed (compatible with our GPL-3-or-later). |
| `.komikku/` | Python / GTK4 | Top-tier native GNOME manga/webtoon reader. Reference for the reader pager logic and chapter handling. (See `.komikku/komikku/reader/pager/`) |

The Phase 13.1 architectural pattern (GListModel of structurally-typed
blocks → GtkSignalListItemFactory → native widget per row) is named
"Fractal-style" in earlier patchnotes (v0.40.0 onward) — that was a
slip; the actual reference Brandon meant was **Foliate** all along.
Patchnotes are historical record and stay as-is; new docs use
"Foliate-style".

### License compatibility matrix

Framework is **GPL-3.0-or-later**. All borrowed-code attributions go in `README.md` "Influences and borrowed techniques"; the SPDX identifier on every Framework source file stays `GPL-3.0-or-later` regardless of source.

| Source | License | Code copy allowed? | Notes |
|---|---|---|---|
| zathura | Zlib | yes | Permissive. Must not misrepresent origin (Zlib §1); attribution preserves required notice. |
| zathura-pdf-mupdf | Zlib | yes | Same. |
| SumatraPDF | GPL-3.0 (source headers say `License: GPLv3`; readme's "(A)GPLv3" wording reflects that the *binary* link with AGPL'd MuPDF is effectively AGPL — the source itself is GPL-3) | yes | Combined work distributable under GPL-3 (the common denominator with GPL-3-or-later). |
| Sioyek | GPL-3.0 | yes | Same as Sumatra. |
| Foliate | GPL-3.0+ | yes | Compatible. |
| foliate-js | MIT | yes | MIT is GPL-compatible; attribution preserved per OFL/MIT conventions. |
| Komikku | GPL-3.0-or-later | yes | Fully compatible with Framework's license. |
| MComix | GPL-2.0+ | yes | Compatible; combined work distributed as GPL-3.0-or-later. |
| YACReader | GPL-3.0 | yes | Same as Sumatra. |
| Plato | **AGPL-3.0** | **NO source copies** | Technique reference only. Copying code would force Framework to AGPL. |
| MuPDF (system dep) | AGPL-3.0 | **NO source copies** | We link the system library — fine. The shipping binary is effectively AGPL because of this link, even though Framework's source stays GPL-3-or-later. |
| DjVuLibre (system dep) | GPL-2-or-later | linking only | Already credited in README dependency table. |

**Implications for distributors:**
- Framework source: GPL-3-or-later (recipient can choose GPL-3 or any later GPL).
- Framework binary as shipped: effectively AGPL-3 due to MuPDF link (corresponding source must be made available).
- If you cherry-pick GPL-3 code from Sumatra or Sioyek into Framework, that *file* should still carry our SPDX header — the original copyright notice goes in `README.md` attribution, not duplicated per-file.

**Do not** copy code from Plato — AGPL is incompatible with Framework's GPL-3-or-later. Pattern-borrow only.
