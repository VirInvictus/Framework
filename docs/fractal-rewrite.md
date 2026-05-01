# Fractal-Style Reflow Rewrite — Design Note

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

This document scopes the EPUB/MOBI/AZW3/FB2/TXT reflow rewrite tracked
as **Phase 13.1 — Fractal-Style EPUB Reflow** in `roadmap.md`. The
rewrite is the largest single architectural change planned for
Framework post-1.0, so this note exists to lock down the scope, the
boundary against the existing fixed-layout pipeline, and the
implementation order *before* a single line of code lands.

The `FwView` + MuPDF + `FwCache` pipeline is not touched. Reflow runs
in parallel as `FwReflowView` + `FwReflowDocument` + native GTK
widgets, dispatched at file-open time.

---

## 1. Format scope

### In scope (primary targets)

| Format | Container | Content | Implementation cost |
|---|---|---|---|
| **EPUB** | ZIP (libarchive) | XHTML + CSS (subset) | High — XML parsing, OPF spine walk, image extraction |
| **MOBI** | PalmDOC | KF7 HTML (legacy Kindle) | High — PalmDOC decompressor (LZ77 variant) |
| **AZW3** | PalmDOC + KF8 | Enhanced HTML5 + CSS | Shares MOBI's PalmDOC path; KF8 parser on top |
| **FB2** | bare XML | FictionBook XML schema | Medium — straight XML walk |
| **TXT** | bare bytes | UTF-8 plain text | Trivial — split on blank lines |

The five formats above span the realistic ebook surface Framework
needs to handle. EPUB, MOBI, and AZW3 are the high-volume targets;
FB2 is included because Brandon already has FB2s in the corpus that
MuPDF handles via reflow today; TXT is a freebie that exercises the
block AST in isolation.

### Out of scope (and why)

| Format | Why skip |
|---|---|
| **LIT** | Microsoft Reader, dead since 2012. MuPDF doesn't ship a backend for it. Calibre converts LIT → EPUB on import; users hit the EPUB path naturally. |
| **CHM** | Compiled HTML help, legacy Microsoft tooling. Niche enough that Foliate doesn't support it either. |
| **DOCX** | Office Open XML. Real document model that needs a Word-grade renderer (tables, fields, comments). Foliate skips it; we should too. |
| **RTF** | Ancient, ambiguous spec, low value. |
| **Raw PDB** | The container under MOBI/AZW3, but pure PDB-as-text-content (no MOBI header) is so rare it's not worth a code path. Falls through to the MuPDF backend if anyone shows up with one. |
| **KFX** | Encrypted modern Kindle format. Out of scope for any open-source reader without DRM circumvention. |

The split between "in" and "out" follows two rules: (1) format is
actually reflowable text, not fixed layout; (2) format has enough
real-world content to justify maintenance.

### What stays on the existing fixed-layout pipeline

PDF, DjVu, XPS, CBZ, CB7, CBT, CBR — all stay on `FwView` + MuPDF /
DjVuLibre / libarchive backends. Nothing changes for those.

---

## 2. Architecture

### The split

```
                  ┌──────────────────────────┐
                  │       FwApplication      │
                  │   open file → dispatch   │
                  └─────────────┬────────────┘
                                │ (MIME / extension)
              ┌─────────────────┼─────────────────┐
              ▼                                   ▼
  ┌──────────────────────┐            ┌──────────────────────┐
  │  Fixed-layout path   │            │  Reflow path  (NEW)  │
  │  FwView + FwCache    │            │  FwReflowView        │
  │  FwDocument(MuPDF/   │            │  FwReflowDocument    │
  │  DjVu/CBR backends)  │            │  (EPUB/MOBI/...)     │
  │  cairo + GdkTexture  │            │  GtkListView + Label │
  └──────────────────────┘            └──────────────────────┘
        UNCHANGED                           BUILT NEW
```

`FwWindow` becomes a thin shell that hosts whichever view widget
matches the open document's category. Most of the menu actions
(Open, Properties, Print, About, Search) work for both; the
zoom/rotation/crop/loupe/ruler set is fixed-layout-only and gets
hidden when the reflow view is active.

### `FwReflowDocument` interface

A new interface parallel to `FwDocument` (we don't extend
`FwDocument` because the vtable has too many fixed-layout-specific
methods like `render_page`, `get_page_size`, `get_links`):

```c
typedef enum {
  FW_BLOCK_HEADING,    /* level encoded in `level` */
  FW_BLOCK_PARAGRAPH,
  FW_BLOCK_BLOCKQUOTE,
  FW_BLOCK_LIST,       /* GArray of children (LIST_ITEMs) */
  FW_BLOCK_LIST_ITEM,
  FW_BLOCK_CODE,       /* preformatted; preserve whitespace */
  FW_BLOCK_IMAGE,      /* href into the document's image table */
  FW_BLOCK_HR,
  FW_BLOCK_CHAPTER,    /* logical break — table-of-contents anchor */
} FwBlockType;

typedef struct {
  FwBlockType type;
  int         level;      /* heading level, list nesting */
  char       *text;       /* Pango markup; NULL for IMAGE/HR/CHAPTER */
  char       *image_id;   /* IMAGE only; key into doc image table */
  char       *anchor_id;  /* CHAPTER/HEADING only; for ToC nav */
  /* ordered/unordered, list-item index, etc. as packed flags */
  guint       flags;
} FwBlock;

#define FW_TYPE_REFLOW_DOCUMENT (fw_reflow_document_get_type ())

struct _FwReflowDocumentInterface {
  GTypeInterface parent_iface;

  /* Lifecycle */
  gboolean     (*open)             (FwReflowDocument *self,
                                    const char *path,
                                    GError **error);
  void         (*close)            (FwReflowDocument *self);

  /* Blocks — flat sequence accessible as a GListModel */
  GListModel  *(*get_block_model)  (FwReflowDocument *self);

  /* Images — opaque cookie used by image_id; backend resolves to
   * GdkTexture. Stays NULL for backends without images (TXT). */
  GdkTexture  *(*get_image)        (FwReflowDocument *self,
                                    const char *image_id);

  /* Chapter / TOC navigation */
  GListModel  *(*get_toc)          (FwReflowDocument *self);   /* TocItem GObjects */
  guint        (*find_block_by_anchor) (FwReflowDocument *self,
                                        const char *anchor_id);

  /* Search — return a GArray of (block_index, char_offset, length) */
  GArray      *(*search)           (FwReflowDocument *self,
                                    const char *needle);

  /* Metadata — GHashTable of free-form (key, value) — title, author,
   * creator, language, modified-date, etc. NULL when none. */
  GHashTable  *(*get_metadata)     (FwReflowDocument *self);
};
```

The block model is the hot path: `FwReflowView` binds it to a
`GtkListView` directly. No virtual file pointers, no per-frame
parsing — the document is fully resolved into a flat block list at
open time.

### `FwReflowView`

Thin GTK widget. Internals:

- One `GtkListView` filling the widget area.
- A `GtkSignalListItemFactory` that maps `FwBlock` → `GtkWidget`:
  - `BLOCK_HEADING` → `GtkLabel` with CSS class `.heading-N`
  - `BLOCK_PARAGRAPH` → `GtkLabel` (`wrap=TRUE`, `xalign=0`)
  - `BLOCK_BLOCKQUOTE` → `GtkLabel` inside a left-padded `GtkBox`
  - `BLOCK_CODE` → `GtkLabel` with monospace markup, `selectable=TRUE`
  - `BLOCK_IMAGE` → `GtkPicture` with the resolved texture
  - `BLOCK_HR` / `BLOCK_CHAPTER` → `GtkSeparator`
  - `BLOCK_LIST` → recursive (factory recurses)
- CSS is loaded once at view init; CSS classes drive typography.
  No per-document CSS — the user's preferences (font size, line
  height, max width) win. This is a deliberate trade vs. respecting
  publisher CSS — Foliate goes the other way.
- Native text selection works because `GtkLabel` has `selectable`
  and Pango handles the cross-block selection. We bind Ctrl+C to
  the standard clipboard action.

### What goes away for reflowed docs

- Zoom slider — replaced with a font-size adjustment.
- Rotation, crop margins, loupe, reading ruler — all fixed-layout
  features. Hidden in the reflow view.
- Page navigation (Page Up/Down) — repurposed as half-page scroll
  rather than absolute page jump.
- Print — Foliate doesn't print either; defer.
- Page count — replaced with a "X% read" or chapter X/N indicator.

---

## 3. Format-specific implementation notes

### TXT (easiest — start here)

- Open the file as UTF-8, fall back to UTF-16 / Latin-1 if BOM /
  invalid sequences detected.
- Split on `\n\n` (or platform variants) to get paragraphs.
- One `BLOCK_PARAGRAPH` per chunk, text = the chunk verbatim
  (escape Pango markup characters).
- Only one image type: none. `get_image` returns NULL always.
- TOC: nothing. `get_toc` returns an empty model.

This serves as the integration test for the entire pipeline before
we touch a real format. If `FwReflowView` works on a 1 MB plain text
file, the GTK side is sound.

### FB2

- Bare XML. Use GLib's `GMarkupParser` (already a Framework dep via
  GTK).
- Walk `body > section`. Each `section` becomes a `BLOCK_CHAPTER`
  marker followed by its content.
- Tags map cleanly: `<title>` → HEADING, `<p>` → PARAGRAPH,
  `<cite>`/`<epigraph>` → BLOCKQUOTE, `<image l:href="#X">` →
  IMAGE referencing the inline `<binary id="X">` (base64 PNG/JPEG).
- The `<binary>` table is parsed once into a hash of
  `image_id → GdkTexture`.
- Metadata under `<description>` is straightforward.

### EPUB (the headline format)

- Open the ZIP via libarchive (already a Framework dep).
- Parse `META-INF/container.xml` to find the `.opf` rootfile.
- Parse the OPF for the manifest (id → href, media-type) and
  the spine (reading order).
- For each spine entry:
  1. Read the XHTML.
  2. Run a tolerant HTML parser (libxml2's HTML mode is the
     practical pick — already in Fedora's GTK dep tree). Tag map:
     `<h1..h6>` → HEADING, `<p>` → PARAGRAPH, `<blockquote>` →
     BLOCKQUOTE, `<ul>`/`<ol>` → LIST, `<li>` → LIST_ITEM,
     `<pre>`/`<code>` → CODE, `<img>` → IMAGE, `<hr>` → HR.
     Inline tags (`<em>`, `<strong>`, `<a>`, `<span>`) become
     Pango markup inside the parent block.
  3. Image hrefs are resolved against the manifest at this stage —
     they get keys like `manifest_id:resource_path` so cross-file
     references resolve.
- TOC from the navigation document (`nav.xhtml` for EPUB 3) or the
  legacy `toc.ncx` (EPUB 2). Both supported.
- CSS is **not** applied. The author's intended typography is lost
  by design — we render with our CSS for consistency and reflow
  reliability. Same call Komikku and Plato make.
- DRM (Adobe ADEPT, etc.) → fall through to a clear error
  message. No circumvention attempts; out of scope.

### MOBI / AZW3

- Same container (PalmDOC). The header tells us which subformat
  (KF7 = MOBI, KF8 = AZW3, KF7+KF8 hybrid is common).
- PalmDOC decompression: LZ77 variant (well-documented; ~100 LOC).
  Foliate's `kindle-unpack` Python is a reference; we'll write C.
- After decompression: KF7 is a flat HTML stream; KF8 is a tarball
  of HTML chapters + a manifest (parallels EPUB structure).
- Once decompressed and parsed, the block AST flow is the same as
  EPUB.
- Hardest of the five — both because of the binary container and
  because real-world MOBI files have edge cases (broken indexing,
  partial KF8 records, etc.). Plan to ship MOBI/AZW3 last.

---

## 4. Implementation phasing

Each phase is its own version bump and can be paused/abandoned
without breaking the previous one.

| Phase | Version | Deliverable |
|---|---|---|
| **0** | (this doc) | Design lock-in; user sign-off |
| **1** | v1.x.0 | `FwReflowDocument` interface; `FwReflowView` widget; `FwReflowDocumentTxt` backend. End-to-end on .txt files. Also wires the open-time dispatch (extension-based for now). |
| **2** | v1.x.0 | `FwReflowDocumentFb2`. FB2 search, TOC, metadata work. |
| **3** | v1.x.0 | `FwReflowDocumentEpub`. The marquee delivery — EPUB 3 (and EPUB 2). Parses navigation, manifest, spine. |
| **4** | v1.x.0 | `FwReflowDocumentMobi`. Includes the PalmDOC decompressor and KF7 path. |
| **5** | v1.x.0 | `FwReflowDocumentAzw3` (KF8 on top of MOBI's PalmDOC). |
| **6** | v1.x.0 | Polish: font-size adjustment, "X% read" indicator, search highlight in the GtkListView, chapter sidebar, fall-through-to-MuPDF toggle for difficult docs. |

Phase 1 is the riskiest because it's where the architecture proves
out. Phases 3–5 are linear from there.

The "fall-through-to-MuPDF toggle" in phase 6 is the escape hatch:
the existing fixed-layout pipeline can still open EPUB/MOBI via
MuPDF's `fz_layout_document`, so a "use legacy renderer" toggle
gives users an out for any document the new path mangles.

---

## 5. Open questions (defer until phase 1 is in flight)

- **Font preferences**: GSettings keys for body font / monospace
  font / size / line-height? Or take the system default and only
  expose size? (Foliate exposes everything; Plato exposes just
  size.) Lean toward Plato's minimal control surface.
- **Two-column desktop layout**: at very wide windows, single-column
  reflow leaves a lot of whitespace. Two-column needs a paginated
  GtkListView extension or a custom layout. Worth the effort?
- **EPUB CSS support, behind a toggle**: the "we render with our
  CSS" decision is opinionated and will draw complaints. A "respect
  publisher CSS (best-effort)" toggle is feasible but expensive —
  needs a CSS subset parser. Park until users ask.
- **Continuous vs. paginated**: Komikku-style infinite vertical
  scroll is the natural GtkListView behavior. Foliate-style
  paginated is more book-like but hostile to GtkListView. Default to
  continuous; revisit if the experience asks for paginated.
- **Print support**: Foliate doesn't print reflowed content (no
  fixed pages). Realistic answer is "no print for reflow mode";
  document and move on.

---

## 6. What this isn't

- Not a Word renderer. DOCX is out.
- Not a CSS engine. Author CSS is dropped by design.
- Not a DRM circumvention path. Encrypted EPUB / KFX → graceful
  error.
- Not a converter. We don't write back, we don't export, we don't
  re-flow into PDF. Read-only viewer, full stop.
- Not a replacement for Foliate. Foliate is a serious ebook reader
  with annotation, sync, and DRM-aware conversion. Framework's
  reflow mode is "open this EPUB cleanly without launching another
  app while I'm in the middle of a research session."

---

## 7. References

- Komikku — `.komikku/` reference (Python/GTK4) for the GtkListView
  reflow pattern. Source of the architecture sketch in section 2.
- Foliate — feature reference but not architecture (their stack is
  WebKitGTK + JavaScript ebook.js; we're going native GTK).
- `.fractal/src/utils/grouping_list_model/` — shapes the dynamic-
  height list-model pattern (Fractal is a chat app but the list
  arch is exactly what reflowed paragraphs want).
- Plato — `.plato/crates/core/src/document/` — the minimum-viable
  format-handling code. Useful negative reference: Plato uses MuPDF
  for everything and we're explicitly leaving MuPDF for these
  formats.
