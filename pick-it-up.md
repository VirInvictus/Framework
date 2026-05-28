<!-- Scratch handoff doc. Untracked. Written 2026-05-28 mid-session
     during Phase 17 WebKit reflow pivot. Delete or refresh once v0.68
     ships. Replaces the v0.67-era pick-it-up.md. -->

# Framework — Pick-It-Up Notes (Phase 17 WebKit pivot)

Context dump for resuming the WebKit reflow renderer work. We pivoted
mid-session away from Phase 16 (typography pillars) after discovering
the existing FwReflowView block-model pipeline doesn't render real
EPUBs even on pristine v0.67.0. Foliate and Calibre both use a web
engine; we followed suit.

---

## 1. Where the bits live

- **Main branch (`main`)**: still at v0.67.0 in `meson.build`. **Working
  tree carries the unreleased Phase 17 work** (WebKitGTK reflow path
  for EPUB). Nothing committed on `main` from this session.
- **`parking/phase-16-hyphenation` (commit `88d2e72`)**: Phase 16
  Pillar 1 (en_US Knuth-Liang hyphenation) preserved off-branch.
  Includes hyph-en-us patterns under `data/hyphenation/`,
  `src/fw-hyphenate.{c,h}`, GSetting, Reading Settings dialog row.
  ASan-clean, verified on real EPUB block text. Do not delete the
  branch.
- **The plan**: `/home/bdkl/.claude/plans/dreamy-jumping-scone.md`.
  Brandon approved it; we're partway through Step 6 (verify) when
  context ran out. Steps 0–4 done, Step 5 (webview state persistence)
  deferred until end-to-end works, Step 6 in progress, Step 7 (bump,
  patchnote, ship) pending.

---

## 2. What's working as of last hand-off

EPUB at `.testfiles/playing-at-the-world-v2.epub`:

- Renders cleanly via WebKitGTK 6.0 (the EPUB Foliate and Calibre
  both handle but the v0.67 reflow pipeline couldn't).
- UTF-8 chars come through correctly (apostrophes, em-dashes, etc.).
- Cover image displays full-viewport on the first scroll position.
- Mouse-wheel scroll, PageDown / arrow keys advance pages cleanly
  after the DMA-BUF workaround landed.
- TOC sidebar routing dispatches via `reflow_via_webview`
  (`fw_webview_scroll_to_anchor` vs the FwReflowView equivalent).
- Search bar (Ctrl+F) drives `WebKitFindController`; count label
  updates via the `search-changed` signal.

What was burned in to get here (these gotchas must not be lost):

1. **WebKit's bwrap sandbox doesn't set up on Brandon's Fedora 44**
   ("bwrap: Failed to make / slave: Operation not permitted").
   Bypass: `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` set via
   `g_setenv` early in `main.c:139`. Justifiable for a
   trusted-document viewer; Brandon's library is his own files.
2. **Landlock EXECUTE drop bricked WebKit's child-process spawn.**
   `fw-sandbox.c` no longer includes `LANDLOCK_ACCESS_FS_EXECUTE` in
   the dropped rights set; only MAKE_* stays. Path-beneath EXECUTE
   allow for `/usr/libexec/webkitgtk-6.0/*` is a Phase 17.x TODO.
3. **DMA-BUF renderer leaks stale frames during continuous scroll on
   Wayland** ("rendering doesn't clear out on every page"). Fix:
   `WEBKIT_DISABLE_DMABUF_RENDERER=1` env var (also `g_setenv` in
   `main.c`). Same workaround Foliate uses. Cost is trivial.
4. **Cover image was rendering as broken-img.** Root cause was
   security-origin mismatch: the document loaded with `base_uri=NULL`
   (about:blank) was cross-origin to `framework-img://...`, so
   WebKit dropped the image fetch before calling our scheme handler.
   Two-line fix:
   - `fw-webview.c` `ensure_uri_scheme_registered`: DROP the
     `register_uri_scheme_as_local` call (it was forcing file-style
     restrictions). Keep `cors_enabled`.
   - `fw-webview.c` `fw_webview_load_html`: pass
     `framework-img://<doc_id>/` as the base URI instead of NULL,
     so the document is same-origin with its image fetches.
5. **EPUB chapters parsed as Latin-1.** `htmlReadMemory` defaults to
   the wrong encoding when the source doesn't have a charset
   declaration. Always pass `"UTF-8"` explicitly (EPUB spec
   guarantees it). Fix in `fw-reflow-document-epub.c` near
   `htmlReadMemory` call inside `epub_produce_html`.

---

## 3. The architecture as of right now

```
                ┌────────────────────────────────────┐
fw_window_open_reflow
  │
  ├── fw_reflow_document_new_for_path  (parsers UNCHANGED — keep)
  │     ↓
  ├── if fw_reflow_document_supports_html(doc):
  │       fw_reflow_document_produce_html → (html, GHashTable<image_id, GBytes>)
  │       fw_webview_load_html(self->webview, html, images)
  │       gtk_stack_set_visible_child_name("webview")
  │       self->reflow_via_webview = TRUE
  │   else:
  │       fw_reflow_view_set_document  (OLD path — unchanged)
  │       gtk_stack_set_visible_child_name("reflow")
  │       self->reflow_via_webview = FALSE
```

Each open call decides per-format. Today only the **EPUB backend** has
`produce_html` wired; the others still go through `FwReflowView`. That
incremental cutover is by design (the plan: EPUB first, then MOBI/AZW3,
then FB2, then TXT, then delete FwReflowView).

### Key files added / changed in this session (all uncommitted on main)

| File | Status | Purpose |
|---|---|---|
| `src/fw-webview.{c,h}` | new | WebKitGTK reader; URI scheme; find controller |
| `src/fw-reflow-document.{c,h}` | modified | Added `produce_html` vtable + `supports_html` accessor |
| `src/fw-reflow-document-epub.c` | modified | Implemented `epub_produce_html`; promoted spine + image bookkeeping to struct state; cover-emit guard |
| `src/fw-window.c` | modified | Dispatch to webview when supported, route TOC anchor + search + Page Up/Down + Ctrl+F |
| `src/fw-sandbox.c` | modified | Dropped EXECUTE from Landlock rights set |
| `src/main.c` | modified | `WEBKIT_DISABLE_SANDBOX_*` and `WEBKIT_DISABLE_DMABUF_RENDERER` env vars |
| `src/meson.build` | modified | `dependency('webkitgtk-6.0', version: '>= 2.46')` |
| `io.github.virinvictus.framework.yml` | modified | Comment about WebKit in GNOME 50 runtime |
| `README.md` | modified | Dep table + Fedora install command updated |

### Cross-cutting routing in `fw-window.c`

Every search / nav code path now branches:
```
if (self->reflow_via_webview && self->webview) → fw_webview_*
else if (self->reflow_doc)                     → fw_reflow_view_*  (legacy)
else                                           → fixed-layout
```

If you add a new search or nav action, follow this triplet. Sites:
`prev/next_page_clicked`, `act_next_page` / `act_prev_page` /
`act_first_page`, `act_find_next` / `act_find_prev`,
`on_search_entry_changed/next/previous`, `search_next/prev_clicked`,
`on_reflow_sidebar_anchor_requested`, key-event handler around
`GDK_KEY_Left/Right`.

---

## 4. What still needs to happen before v0.68.0 ships

In priority order.

### 4a. Sanity sweep (Step 6, in progress)

Confirm the non-EPUB reflow formats still work via the legacy path
(`reflow_via_webview = FALSE`):

- `.testfiles/the-broken-god.mobi` — MOBI / KF7
- `.testfiles/datapoint.azw3` — AZW3 / KF8
- A FB2 (corpus has none; the parser is otherwise covered by
  stress-reflow)
- A TXT (test against any plain text file)

And confirm fixed-layout PDF / DjVu / CBR are untouched. The plan's
verify section lists the full pass.

Run the stress suite (`meson test -C builddir`) under ASan+UBSan:

```sh
meson configure builddir -Dsanitize=address,undefined
meson compile -C builddir
GSETTINGS_SCHEMA_DIR=builddir/data \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  meson test -C builddir
meson configure builddir -Dsanitize=
```

`stress-corpus-soak` is known to overrun its RSS cap under ASan
(documented in memory `project_corpus_soak_asan_cap`); 5/6 passing is
the target. Anything else regressed needs root-cause before shipping.

### 4b. Webview state persistence (Step 5, skipped during verify)

The plan was: extend `fw_state.c` with a `webview_pos` JSON field
(`{"anchor": "...", "scroll_y": N}`). `fw_webview_get_position` is
already implemented and returns this shape via a JS one-shot (see
`fw-webview.c`). On save: call it before `fw_window_close_active_document`,
write the JSON into state. On open: read the JSON, call
`fw_webview_restore_position(json)` which is already wired (it queues
until `load_done` if the load hasn't finished).

If the state schema gains a new field, `fw_document_state_free` and
the JSON readers/writers in `fw-state.c` need the corresponding
allocation/free dance. Mirror the `reflow_block` path verbatim.

### 4c. Auto-reload monitor

The existing `fw_window_start_monitor` path watches the document for
external changes (LaTeX flow). It currently only re-opens via the
fixed-layout pipeline. The reflow path doesn't reuse the monitor at
all (see `fw_window_open_reflow` end: comment "No file-monitor /
state-restore in Phase 1"). Acceptable for v0.68; flag as a known
deferral in the patchnote.

### 4d. README / patchnotes / roadmap / version bump

- `meson.build` → `version: '0.68.0'`
- `patchnotes.md`: prepend v0.68.0 entry. Suggested shape:
  > **v0.68.0: EPUB reflow via WebKitGTK.** Phase 17 begins. EPUB
  > content now renders through WebKitGTK 6.0 instead of the
  > FwReflowView block-model pipeline; foliate-js parsers (OPF
  > walking, spine, NCX, image manifest) stay in place and feed
  > stitched HTML to the new FwWebView. Cover, TOC navigation, search,
  > styling, hyphenation, OpenType features, and theme support all come
  > "for free" from WebKit's renderer. Other reflow formats
  > (MOBI / AZW3 / FB2 / TXT) still use the legacy reflow path until
  > they get their own `produce_html` implementations. Phase 16
  > typography pillars (hyphenation, OpenType features, themes,
  > measure) parked on `parking/phase-16-hyphenation`; most of that
  > work becomes CSS the WebView already does natively.
- `roadmap.md`: add Phase 17. Tick "EPUB produce_html". List
  remaining backends as TODO sub-items. Mark old Phase 16 superseded
  (link to parking branch).
- `README.md`'s "Dependencies" table already updated. The
  "Influences and borrowed techniques" section should get a Foliate
  shout-out specifically for the WebKit pattern (foliate-js parsers,
  WebKit renderer).

### 4e. Commit and tag

One PR-style commit for the WebKit pivot, plus the version-bump
commit. Show messages before committing per the house rule. Don't
push without explicit approval.

---

## 5. Open follow-up issues (not v0.68 blockers)

- **Landlock EXECUTE allowlist**: bring EXECUTE drop back behind a
  path-beneath rule allowing `/usr/libexec/webkitgtk-6.0/*` and
  `/usr/lib*/` so the bubblewrap sandbox restoration doesn't sacrifice
  defense-in-depth. The Phase 17.x slot.
- **CSS link rewriting**: `epub_produce_html` currently strips
  `<link rel="stylesheet">`. We rely on the built-in `EPUB_READING_CSS`
  block instead. To honour publisher CSS, add a `framework-css:` scheme
  that serves CSS bytes and rewrites `url()` references inside the CSS
  too. Probably worth one round of "do users notice?" before doing it.
- **Cross-chapter href links**: anchor refs like
  `chapter_11.xhtml#footnote-441` aren't rewritten to in-doc
  fragments. Within a stitched spine they should become `#footnote-441`
  (anchor IDs are preserved verbatim). Trivial regex pass during the
  produce_html walk.
- **Multi-window doc-id collision**: registry is global; doc_ids are
  random 8-hex-byte tokens. Collision probability is ~0 but flag is
  not actually checked. Defensive belt-and-suspenders if/when we hit
  multi-window scenarios under stress.
- **JS execution surface**: `enable_javascript=TRUE` is needed for
  `evaluate_javascript` (scroll, anchor, position). EPUB content can
  itself ship `<script>`, which we strip during the HTML emit
  (`process_html_subtree` removes script elements). Verify the
  stripping is exhaustive (inline `onclick=` attributes for example
  are not stripped; consider scrubbing those too).

---

## 6. Cheat-sheet

```sh
# Rebuild
meson compile -C builddir

# Run on the test EPUB (cover should fill viewport, scroll-by-page clean)
GSETTINGS_SCHEMA_DIR=builddir/data \
  ./builddir/src/framework .testfiles/playing-at-the-world-v2.epub

# Run on a MOBI (should still use the legacy FwReflowView; sanity check)
GSETTINGS_SCHEMA_DIR=builddir/data \
  ./builddir/src/framework .testfiles/the-broken-god.mobi

# Run a fixed-layout PDF (untouched path)
GSETTINGS_SCHEMA_DIR=builddir/data \
  ./builddir/src/framework .testfiles/effective-java.pdf

# Stress suite (ASan+UBSan)
meson configure builddir -Dsanitize=address,undefined
meson compile -C builddir
GSETTINGS_SCHEMA_DIR=builddir/data meson test -C builddir
meson configure builddir -Dsanitize=

# If GSettings schema changes, recompile manually
glib-compile-schemas --targetdir=builddir/data data
```

The two diagnostic-during-development env vars (`WEBKIT_DISABLE_DMABUF_RENDERER`,
`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS`) are set inside `main.c` with
`g_setenv(..., FALSE)` — the FALSE means user env takes precedence. So
you can override them if needed.

---

## 7. Tasks status

The session task list (open tasks at hand-off):

- **#15 State persistence for webview position** (pending) — Step 5
- **#16 Verify EPUB renders + sanity sweep** (in progress; EPUB
  itself confirmed; non-EPUB sanity sweep + ASan stress run remaining)
- **#17 Ship v0.68.0** (pending) — Step 7

Tasks #1–14 done. Branch parking work captured at `88d2e72`.

---

## 8. Reset on resume

First thing tomorrow: run `git status` and `git log --oneline -5` to
confirm what state you're in. Re-read the plan
(`/home/bdkl/.claude/plans/dreamy-jumping-scone.md`) for the Step 5/6/7
contract. Then pick up at the sanity sweep — run the MOBI/AZW3/PDF
smoke tests, then the ASan suite. Once everything is green, do Step 5
(state persistence) then Step 7 (ship).
