<!-- Scratch handoff doc. Written 2026-05-28, refreshed after v0.77.0.
     Delete or refresh when the next chunk lands. -->

# Framework: Pick-It-Up Notes (after v0.77.0)

Resume point for the reflow / WebKit cleanup work. Everything below is on
`main` unless stated otherwise.

---

## 1. Where things stand

- **Branch / version:** `main`, at **v0.77.0**. (Confirm push state with
  `git log --oneline origin/main..HEAD`.) Every reflow format (EPUB /
  MOBI / AZW3 / FB2 / TXT / Markdown) renders through `FwWebView`
  (WebKitGTK). The legacy `FwReflowView` block-model renderer is **gone**.
- **Parked branch:** `parking/phase-16-hyphenation` (`88d2e72`) still
  exists. en_US Knuth-Liang hyphenation for the *native* reflow path,
  superseded by WebKit (CSS `hyphens: auto`). Don't delete; only copy.
- **Recent arc (all shipped):**
  - **v0.68.0** EPUB via WebKitGTK 6.0 (`FwWebView`).
  - **v0.69.0** fit-width median-body-page normalization + variance guard;
    Ctrl+Q/Ctrl+W save state for every format; reflow HR CSS fix.
  - **v0.70.0** comic DPI height-normalization (the nausicaa fix).
  - **v0.71.0** EPUB reading typography: serif default (Crimson Pro),
    Light/Sepia/Kanagawa-Dark/Follow-System themes, bundled-font picker,
    live CSS.
  - **v0.72.0** MOBI/AZW3 via WebKit (Phase 17.2): shared `fw-reflow-html`;
    `mobi_produce_html`; KF7/KF8 image resolution; synthetic cover.
  - **v0.73.0** FB2 via WebKit (Phase 17.3): `fb2_produce_html` (xmlDoc →
    HTML).
  - **v0.74.0** TXT via WebKit (Phase 17.4): `txt_produce_html`.
  - **v0.75.0** Markdown (net-new): `FwReflowDocumentMd` + vendored md4c
    (MIT, `src/md4c/`, GitHub dialect, `MD_FLAG_NOHTML`).
  - **v0.76.0** Phase **17.5a**: deleted `FwReflowView`
  - **v0.77.0** Phase **17.5b**: Stripped the block-model machinery entirely. Reflow interface no longer mentions FwBlock.
    (`fw-reflow-view.{c,h}` gone); unified `fw-window.c` on the single
    `"webview"` reflow path (dual stack pages, parallel nav/search/save
    branches, and `reflow_hits` / `reflow_active` / `reflow_search_*` /
    `on_reflow_page_changed` all removed). Full doc refresh +
    Calibre/md4c influence + license audit landed in the same window.

---

## 2. Architecture quick-reference (current truth)

### Render paths
- **Fixed-layout** (`FwView` + `FwCache` + MuPDF/DjVu/libarchive): PDF,
  DjVu, CBZ/CB7/CBT (MuPDF), CBR (libarchive), XPS.
- **Reflow** (`FwReflowDocument` backend `produce_html` → `FwWebView`):
  EPUB, MOBI, AZW3, FB2, TXT, Markdown. Window dispatch in
  `fw_window_open_file` → `fw_reflow_path_is_supported` →
  `fw_window_open_reflow`; bails to fixed-layout (`fw_document_new_for_path`)
  only if `produce_html` fails.

### Comic backend routing (settled)
- CBZ/CB7/CBT → MuPDF (fast random access, 8 parallel instances).
- CBR (RAR) → libarchive (`fw-document-cbr.c`, serialized; MuPDF can't
  decode RAR). **Do NOT route CBZ to libarchive**; tried it, the
  serialized backend can't keep up with the velocity cache (white pages).
- Comic DPI normalization (v0.70, `fw-document-pdf.c`): median page height
  → per-page `norm_scale` folded into render zoom. fit-width variance
  guard in `fw-view.c` is the backstop.

### Reading typography (live CSS)
- `:root` custom properties (`--body-font` default Crimson Pro,
  `--font-size`, `--line-height`, `--measure`, `--fg`/`--bg`/`--link`) in
  the shared reading CSS. `fw_webview_set_reading_style` pushes them via
  JS `setProperty` (immediate if loaded, else `pending_style` flushed on
  load-finished). Window `apply_reading_style` resolves the `reading-theme`
  enum (system/light/sepia/dark; dark = Kanagawa `#181616`/`#c5c9c5`/
  `#8ba4b0`, follows `AdwStyleManager` for "system"). Reading Settings
  dialog drives it; Ctrl+/Ctrl- adjust `reading-font-size`.

---

## 3. Next step: Phase 17.x

**Goal:** Security tightening and publisher CSS.
do it as its own increment with his visual sign-off; rewrite the stress
test to drive `produce_html` (keep coverage, don't delete it).

**What's dead (only consumer left is `tests/stress/stress-reflow.c`):**
- Interface vtable slots: `get_block_model`, `get_image`,
  `find_block_by_anchor`.
- Wrappers/types in `fw-reflow-document.{c,h}`: `fw_reflow_document_search`,
  `FwReflowHit`, the `FwBlock` GObject (+ all `fw_block_*` accessors),
  `fw_reflow_document_get_block_model` / `_get_image` /
  `_find_block_by_anchor`.
- Each backend's block-building walk + the `blocks` GListStore field.

**Entanglement map (verified; this is why it's not pure deletion):**
- `md`: clean. `produce_html` runs md4c on raw bytes; `blocks` is an empty
  stub. Just drop the stub.
- `fb2`: clean. `produce_html` walks the retained `xmlDoc`; blocks are
  built separately and unused by it. Delete the block walk.
- `txt`: **coupled.** `txt_produce_html` iterates `self->blocks`
  (line ~235). Rewrite it to walk the raw UTF-8 text directly
  (`build_blocks_from_text`'s blank-line split is the logic to lift).
- `epub`, `mobi`: **coupled.** One parse loop populates BOTH the block
  model AND `produce_html`'s data (epub: `spine_paths` + image tables,
  see line ~1165 "Same iteration also populates the produce_html-side
  tables"; mobi: the marker-injected body, line ~742). Surgically keep the
  produce_html-side population, drop only the `g_list_store_append(blocks…)`
  half and the `blocks` field.

**Green-at-every-step order (keeps the build compiling throughout):**
1. Per backend, separate `produce_html` from blocks, then delete that
   backend's block walk + `get_block_model`/`get_image`/
   `find_block_by_anchor` impls + their `iface->` assignments + the
   `blocks` field. (Vtable still *declares* the slots; NULL slots are
   fine, build stays green.) Do md/fb2 first (trivial), then txt, then
   epub/mobi (verify the live render after each).
2. Rewrite `stress-reflow.c` to assert on `produce_html`: non-empty,
   well-formed-ish HTML (has `<body`/`</html>`), and a populated image
   table for image-bearing formats. Drop the `get_block_model` walk +
   `fw_reflow_document_search` assertions.
3. Remove the slots from the interface struct, the wrapper functions, the
   `fw_reflow_document_search` impl, `FwReflowHit`, and `FwBlock` +
   accessors from `fw-reflow-document.{c,h}`.
4. **Verify:** stress 6/6 + ASan 5/6 (corpus-soak RSS overrun is the
   known-benign ASan case). Brandon eyeballs EPUB/MOBI/AZW3/FB2/TXT/MD
   render (headless can only confirm "it opened via webview", not pixels).

**Keep:** `FwReflowSidebar` (reflow TOC, unrelated to the renderer),
`produce_html`, `get_toc`, `get_metadata`, `supports_html`, the raw-image-
bytes retention each backend needs for `produce_html`'s `out_images`.

**Doc sync after 17.5b lands:** CLAUDE.md §"Reflow Pipeline" still lists
the dead slots as "dead-but-still-present (17.5b pending)"; flip that to
gone. roadmap line for 17.5b → `[x]`.

### Other open Phase 17.x work (after 17.5b)
- **Security tightening:** restore the Landlock EXECUTE drop behind a
  path-beneath allow for `/usr/libexec/webkitgtk-6.0/*`; scrub inline
  event-handler attrs (`onclick=`) during HTML emit; revisit the disabled
  WebKit bubblewrap sandbox (`WEBKIT_DISABLE_SANDBOX_*` in `main.c`).
- **Publisher CSS + cross-chapter links:** `framework-css:` scheme;
  rewrite cross-chapter hrefs to in-doc fragments.
- **Auto-reload for the WebView path** (deferred since v0.68): the
  `GFileMonitor` reload only covers fixed-layout. Flag in
  `fw_window_open_reflow`.
- **Markdown follow-ups:** local image refs (`![](pic.png)`) don't load;
  code blocks aren't syntax-highlighted.

---

## 4. Verification cheat-sheet

```sh
meson compile -C builddir
GSETTINGS_SCHEMA_DIR=builddir/data ./builddir/src/framework <file>
glib-compile-schemas --targetdir=builddir/data data   # after gschema edits

# Stress suite (normal: 6/6)
GSETTINGS_SCHEMA_DIR=builddir/data meson test -C builddir

# ASan+UBSan (target 5/6: corpus-soak RSS overrun benign, failures=0)
meson configure builddir -Dsanitize=address,undefined
meson compile -C builddir
GSETTINGS_SCHEMA_DIR=builddir/data \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 meson test -C builddir
meson configure builddir -Dsanitize=
```

**Headless GUI testing (Wayland box, no input-injection):**
- Launch with `FW_DEBUG=1`, read `FW_TRACE_*`. A successful reflow open
  logs `[window] open_file (reflow) done: '<path>'`; a fixed-layout fall
  logs plain `[window] open_file: '<path>'` + `[view] set_document`.
- Graceful save (window close handler) over D-Bus:
  `gdbus call --session --dest io.github.virinvictus.framework
  --object-path /io/github/virinvictus/framework
  --method org.gtk.Actions.Activate "quit" "[]" "{}"`
  (SIGTERM/SIGKILL do NOT save).
- Kill stale instances with `pkill -9 -x framework` (NOT `pkill -f
  src/framework`, which matches the Bash tool's own shell).
- Per-doc state: `~/.local/share/framework/state.json`.
- **Can't verify rendered pixels headlessly** (themes, fonts, sizes).
  Brandon's eyes. Bit us twice (nausicaa).

---

## 5. Test assets

- `.testfiles/` (gitignored): `playing-at-the-world-v2.epub`,
  `the-broken-god.mobi`, `datapoint.azw3`, `effective-java.pdf`,
  `on-growth-and-form.djvu`, `nausicaa-v01.cbz` (mixed-DPI),
  `vagabond-v01.cbr`, `visual-explanations-tufte.pdf`.
- No FB2 / Markdown file in the corpus. FB2 verified on a hand-built
  `/tmp/test.fb2`, TXT on `/tmp/test.txt`, MD likewise. Want real files
  when they turn up.
- Comics: `/mnt/SharedData/Comics/`. Vinland Saga v12 is a mild-mixed-DPI
  case (11/372 outlier pages). Sample with
  `fd -e cbz . /mnt/SharedData/Comics | shuf | head`, never iterate all.
- PDF/DjVu: `/home/bdkl/docs/Calibre Library/` (mind the spaces).

---

## 6. First thing on resume

`git status` + `git log --oneline -5`. Re-read this doc and `roadmap.md`
Phase 17. The pickup is **Phase 17.5b** (§3 above). It's broad and touches
the live render path. Brandon already greenlit doing it as its own
verified increment, so just execute the green-at-every-step order and get
his eyes on the render before shipping.
