<!-- Scratch handoff doc. Written 2026-05-28, refreshed after v0.72.0.
     Delete or refresh when the next chunk lands. -->

# Framework: Pick-It-Up Notes (after v0.72.0)

Resume point for the EPUB / reflow / comic work. Everything below is on
`main` and pushed unless stated otherwise.

---

## 1. Where things stand

- **Branch / version:** `main`, clean working tree, at **v0.72.0**,
  pushed to `origin/main`.
- **Parked branch:** `parking/phase-16-hyphenation` (`88d2e72`) still
  exists. en_US Knuth-Liang hyphenation for the *native* reflow path,
  superseded by WebKit (CSS `hyphens: auto`). Don't delete; it's the only
  copy.
- **This session's arc (all shipped + pushed):**
  - `0fc8f24` **v0.68.0** EPUB renders via WebKitGTK 6.0 (`FwWebView`);
    webview reading-position persistence.
  - `ffdfa3b` **v0.69.0** fit-width page-size normalization (median body
    page) + variance guard; Ctrl+Q/Ctrl+W now saves state for every
    format; reflow HR percentage-margin CSS fix.
  - `0b8e903` **v0.70.0** comic DPI height-normalization (the nausicaa
    "tiny pages" fix).
  - `d5be45c` **v0.71.0** EPUB reading typography: serif default (Crimson
    Pro), Light/Sepia/Kanagawa-Dark/Follow-System themes, bundled-font
    picker, all applied live through the WebView.
  - **v0.72.0** MOBI/AZW3 render via WebKit (Phase 17.2): shared
    `fw-reflow-html` module; `mobi_produce_html`; parser retains raw image
    bytes; img refs resolved for KF7 zero-padded `recindex` + KF8 base-32
    `kindle:embed:`; synthetic cover for EXTH-only covers. MOBI/AZW3
    inherit the v0.71 typography + themes.

---

## 2. Architecture quick-reference (current truth)

### Comic backend routing (settled this session)
- **CBZ / CB7 / CBT -> MuPDF backend** (`fw-document-pdf.c`). Fast random
  access; 8 parallel render instances.
- **CBR (RAR) -> libarchive backend** (`fw-document-cbr.c`). MuPDF can't
  decode RAR. Serialized rendering (one archive lock, one fz_context).
- **Do NOT route CBZ to libarchive.** Tried it this session; the
  serialized backend can't keep up with the velocity cache during
  scrolling -> pages go white. Reverted. The DPI motivation for moving it
  was solved in MuPDF instead (see below).

### Comic DPI normalization (v0.70, `fw-document-pdf.c`)
- MuPDF sizes image pages by each image's embedded DPI. Inconsistent DPI
  metadata (scanlations) makes same-pixel pages report wildly different
  point sizes -> they render at different scales (one full-size, next a
  thumbnail).
- Fix: `pdf_open` detects comic formats (CBZ/CB7/CBT by extension) and
  normalizes every page to the **median page height** (aspect ratio is
  DPI-invariant). Stores per-page `norm_scale`, folded into the render
  zoom in `pdf_render_page` so the texture matches. `get_page_size`
  returns normalized sizes. PDFs/XPS untouched (`norm_scale` NULL).
- No-op for uniform-DPI comics; corrective for outlier pages. Verified on
  nausicaa-v01 and Vinland v12 (11/372 pages were outliers).

### fit-width normalization (v0.69, `fw-view.c`)
- `recompute_layout` computes `typical_width` = median width of body
  pages (excludes page 0 cover + spreads via `view_page_is_spread`).
  `fw_view_fit_width_zoom` keys off it. **Variance guard:** if the body
  sample's max/median > 2.0, sizing is untrustworthy -> set
  `typical_width = 0` -> fall back to fitting page 0. (Now mostly a
  backstop since the DPI normalization makes comic sizes consistent.)

### CBR background dimension probe (v0.69, `fw-document-cbr.c`)
- CBR defaults all pages to the cover's pixel size at open; a one-shot
  GTask worker decompresses the archive once for real per-page dims, then
  emits the new `FwDocument::geometry-changed`. The window
  (`on_doc_geometry_changed`) relayouts + re-fits if `fit_width_active`.
  GTask refs the doc so it outlives the probe; `cbr_close` sets
  `cancel_flag`.

### EPUB typography (v0.71, the live-CSS mechanism)
- `EPUB_READING_CSS` (in `fw-reflow-document-epub.c`) now defines `:root`
  custom properties: `--body-font` (default `'Crimson Pro', Georgia,
  serif`), `--mono-font`, `--font-size`, `--line-height`, `--measure`,
  `--fg`, `--bg`, `--link`.
- `fw_webview_set_reading_style(self, FwReadingStyle*)` builds a JS
  snippet that `setProperty`s those on `document.documentElement`. Runs
  immediately when loaded, else stored in `pending_style` and flushed on
  load-finished (latest wins). Live, no reopen.
- Window: `apply_reading_style()` reads GSettings, resolves
  `reading-theme` enum to colors (`reading_theme_colors`, follows
  `AdwStyleManager` dark for "system"; dark = Kanagawa Dragon
  `#181616`/`#c5c9c5`/`#8ba4b0`), calls the setter. Called after
  `load_html`; re-applied on `changed::reading-*` and
  `notify::dark`. Ctrl+/Ctrl- font-size shortcuts write
  `reading-font-size` -> picked up live.
- Reading Settings dialog: Appearance > Theme combo (enum index == enum
  value) and Fonts > Reading font combo (`READING_FONT_FAMILIES`, the 3
  bundled OFL families); free-text family entry kept for custom fonts.
- `reading-theme` enum in the gschema: system / light / sepia / dark.

---

## 3. Next steps (pick one)

Priority-ish order; all are Phase 17.x in `roadmap.md`.

1. **FB2 -> `produce_html`** (17.3). The next format to migrate. The
   libxml2 FB2 walker (`fw-reflow-document-fb2.c`) currently emits the
   block model; give it a `produce_html` that emits stitched HTML +
   image table (FB2 images are base64 `<binary>` elements referenced by
   `l:href="#id"`). Mirror `mobi_produce_html` / `epub_produce_html` and
   reuse the shared `fw-reflow-html` module + its img-resolver callback.
   Then **TXT -> `produce_html`** (17.4): trivial wrap in `<pre>` or
   paragraph-per-blank-line.
   (DONE v0.72.0: **MOBI / AZW3 -> `produce_html`**, Phase 17.2; see
   `mobi_produce_html` and the shared `fw-reflow-html` module.)
3. **Delete `FwReflowView`** (17.5) once all four reflow formats are on
   WebKit. Drops `get_block_model` / `get_image` / block-model search and
   the renderer halves of each backend. Big cleanup.
4. **Security tightening** (17.x): restore the Landlock EXECUTE drop
   behind a path-beneath allow for `/usr/libexec/webkitgtk-6.0/*`; scrub
   inline event-handler attributes (`onclick=` etc.) during HTML emit;
   revisit the disabled WebKit bubblewrap sandbox
   (`WEBKIT_DISABLE_SANDBOX_*` in `main.c`).
5. **Publisher CSS + cross-chapter links** (17.x): `framework-css:`
   scheme to honour publisher stylesheets; rewrite cross-chapter hrefs to
   in-doc fragments.

### Smaller / deferred
- **Auto-reload for the WebView path** (deferred since v0.68): the
  `GFileMonitor` reload only covers fixed-layout. Flag is in
  `fw_window_open_reflow`.
- **Parallelize the libarchive (CBR) backend** (someday-maybe): clone
  `fz_context` + multiple readers like the PDF backend, to fix CBR
  throughput. Only payoff is tidiness + faster CBR scrubbing; the only
  thing it would unlock is unifying all comics on one backend. Not worth
  it yet.
- **Themes for the legacy reflow formats** (MOBI/AZW3/FB2/TXT): skipped
  intentionally; they're slated for deletion in 17.5, so better to let
  them inherit themes when they move to WebKit (step 1-2 above).

---

## 4. Verification cheat-sheet

```sh
# Build (standardize on builddir)
meson compile -C builddir

# Run (GSettings schema dir is required in dev)
GSETTINGS_SCHEMA_DIR=builddir/data ./builddir/src/framework <file>

# Recompile schema after gschema edits
glib-compile-schemas --targetdir=builddir/data data

# Stress suite (normal: 6/6)
GSETTINGS_SCHEMA_DIR=builddir/data meson test -C builddir

# Stress under ASan+UBSan (target 5/6: corpus-soak RSS overrun is the
# known-benign ASan case, failures=0, zero sanitizer errors)
meson configure builddir -Dsanitize=address,undefined
meson compile -C builddir
GSETTINGS_SCHEMA_DIR=builddir/data \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 meson test -C builddir
meson configure builddir -Dsanitize=
```

**Headless GUI testing (this is a Wayland box, no input-injection tools):**
- Launch with `FW_DEBUG=1`, read `FW_TRACE_*` traces (add temporary ones,
  remove before commit).
- Trigger a *graceful* save (window close handler) over D-Bus:
  `gdbus call --session --dest io.github.virinvictus.framework
  --object-path /io/github/virinvictus/framework
  --method org.gtk.Actions.Activate "quit" "[]" "{}"`
  (plain SIGTERM/SIGKILL do NOT save; `app.quit` now closes windows so it
  does).
- Per-doc state file: `~/.local/share/framework/state.json`. Clear
  `.testfiles/` entries between runs so a restored zoom doesn't mask a
  fresh fit-width computation.
- **Can't verify rendered pixels headlessly** (themes, fonts, page sizes,
  scroll). Those need Brandon's eyes. This bit us twice this session
  (nausicaa).

---

## 5. Test assets

- `.testfiles/` (gitignored): `playing-at-the-world-v2.epub`,
  `the-broken-god.mobi`, `datapoint.azw3`, `effective-java.pdf`,
  `on-growth-and-form.djvu`, `nausicaa-v01.cbz` (mixed-DPI test case),
  `vagabond-v01.cbr`, `visual-explanations-tufte.pdf`.
- Comics library: `/mnt/SharedData/Comics/` (Berserk, Vinland Saga,
  Vagabond, Watchmen, etc.). Vinland Saga v12 is a good mild-mixed-DPI
  case (11/372 outlier pages). Per the corpus rule, sample with
  `fd -e cbz . /mnt/SharedData/Comics | shuf | head`, never iterate all.
- PDF/DjVu corpus: `/home/bdkl/docs/Calibre Library/` (mind the spaces).

---

## 6. First thing on resume

`git status` + `git log --oneline -5` to confirm state. Re-read this doc
and `roadmap.md` Phase 17. The obvious pickup is **step 1 (MOBI/AZW3
produce_html)** unless Brandon points elsewhere. Plan it and get sign-off
before coding (non-trivial).
