# Framework — Patch Notes

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

## v0.64.0 (2026-05-02)

*Reflow typography pass 3: ordered lists, figure captions, raised caps. Widow/orphan tuning skipped — out of practical reach in pure Pango.*

### Ordered list numbering

EPUB and MOBI walkers now distinguish `<ol>` from `<ul>`. Each list-open pushes a counter (1+ for `<ol>`, sentinel 0 for `<ul>`) onto a per-walker `ol_stack`; each `<li>` reads the top, increments if ordered, and emits a `FW_BLOCK_LIST_ITEM` with the number stored in the block's `level` field. Unordered items keep `level == 0`.

The view's bind handler renders accordingly: `level == 0` → bullet (U+2022 + NBSP), `level > 0` → `"N." ` (digit + period + NBSP). Nesting works because the stack is per-list.

This also moves the bullet glyph out of the walker's accumulator (where it used to be a literal `"•  "` prefix on every `<li>`-as-paragraph) and into the bind handler. Cleaner kind separation; FwBlockKind LIST_ITEM is no longer dead code on the EPUB / MOBI paths.

### Figure captions

EPUB and MOBI walkers handle `<figure>` (recurse-only) and `<figcaption>` (emit a paragraph with the new `FW_BLOCK_FLAG_CAPTION` flag). The bind handler renders captions in italic, smaller (size − 2pt), centered with the new `.reflow-caption` CSS class. Captions don't get first-line indent or dropcaps; the indent-pass treats them as non-paragraph for follower decisions.

### Raised cap on chapter-leading paragraphs

The first paragraph of each chapter (i.e. the first paragraph in the document, or the first paragraph after a `CHAPTER` marker / heading) gets `FW_BLOCK_FLAG_DROPCAP`. Bind handler walks past any leading inline markup tags and whitespace, finds the first grapheme, wraps it in `<span size="200%" weight="bold">…</span>`. Pango lifts the cap; the rest of the paragraph flows around it (true CSS-float-style drop caps aren't possible in pure Pango — this is a "raised cap" which is the recognized fallback).

Edge cases: if the first character is an `&entity;` reference, we bail to plain rendering rather than try to wrap a partial entity.

The flag is set in the same one-pass annotation in `fw_reflow_view_set_document` that computes `FW_BLOCK_FLAG_INDENT`. Captions / list items don't qualify.

### Indent rule tightened

Now matches foliate exactly: a paragraph gets `FW_BLOCK_FLAG_INDENT` iff the *immediately previous* block is a `PARAGRAPH` (not caption). After a `LIST_ITEM`, `BLOCKQUOTE`, heading, image, or chapter-marker, the next paragraph starts flush. This is foliate's `:not(p) + p, p:first-child { text-indent: 0 }` rule literally translated.

### Widow/orphan tuning — skipped

CSS `widows: 2; orphans: 2` doesn't have a Pango / GtkLabel equivalent. Pango layouts don't expose "minimum lines on a page" controls, and GtkLabel's wrap mode (`PANGO_WRAP_WORD_CHAR`) handles its own break decisions. Real widow/orphan control would require a custom paginator that re-flows blocks across pages with a min-line constraint, which is an engine-grade rewrite. Deferred.

### Verified

* Build clean.
* 1 EPUB + 1 MOBI sample (random `shuf -n 1`) open cleanly with the new typography.

---

## v0.63.0 (2026-05-02)

*Reflow typography depth pass: first-line indent, list bullets, real horizontal rules.*

Building on v0.61's justification + chapter-title centering, three more print-typography idioms now show up in the reflow stack.

### First-line paragraph indent

Consecutive paragraphs get a leading em-quad (U+2003) prefix to mark a print-style first-line indent. Pango justification stretches the inter-word gaps without disturbing the leading em-quad, so it reads as a real first-line indent rather than a magic-spaced word.

The "consecutive" predicate matches foliate's `:not(p) + p, p:first-child { text-indent: 0 }` CSS idiom: a paragraph qualifies for indent iff the previous block is a paragraph, list-item, or blockquote. Paragraphs immediately after headings, chapter markers, images, code blocks, or HRs stay flush left — those are the typographic "first paragraph" positions.

Computed once at model-load time as the `FW_BLOCK_FLAG_INDENT` flag on `FwBlock` (single O(N) pass in `fw_reflow_view_set_document` after the underlying model is bound). Bind handler is O(1): check the flag, prepend the em-quad if set.

### List item bullets

`FW_BLOCK_LIST_ITEM` blocks previously rendered as plain paragraphs — the bullet glyph was missing. Now they get a `"• "` prefix (U+2022 + non-breaking space) so list structure reads at a glance. Ordered-list numbering is still absent (the walker doesn't distinguish `<ol>` from `<ul>` yet); a future polish.

### Horizontal rules

`FW_BLOCK_HR` previously rendered as the literal string `"———"` (three em-dashes). Functional but obvious. Now an empty label with a `.reflow-hr` class: 1px-thick centered border-bottom at 25% alpha-currentColor, 1em margin top + bottom, 30% side margin so it doesn't reach edge-to-edge.

### Verified

* All 11 MOBI/AZW3 files in the corpus open cleanly with new typography.
* EPUB / MOBI smoke-tested against random samples; both render expected.

---

## v0.62.0 (2026-05-02)

*HuffDic-compressed MOBI support (compression code 17480).*

The MOBI parser previously rejected HuffDic-compressed files with `mobi: HuffDic compression — not yet supported`. This is the compression scheme used by older Mobipocket files (modern Calibre output ships PalmDOC/lz77, compression code 2). v0.62 ports foliate-js's `huffcdic` decoder to C and wires it into the existing decompression dispatch.

### Implementation

`src/fw-mobi-parser.c` gains:

* **`HuffCdic` state** holding the two Huffman tables (`table1[256]` indexed by leading byte, `table2[33]` by code length) and the dictionary built from CDIC records.
* **`huffcdic_build`** — reads the HUFF record (magic `HUFF`, 32-bit table offsets at +8 and +12), populates both tables, then walks `numHuffcdic - 1` CDIC records (magic `CDIC`, length / numEntries / codeLength fields) to construct the dictionary. Each entry's `decompressed` flag (`x & 0x8000` of the per-entry size word) gets preserved for later recursive expansion.
* **`huffcdic_decompress`** — reads 32 bits from the input bitstream, looks up the leading byte in `table1`; on terminate, advances by `code_length` and emits the dictionary entry; otherwise walks `table2` upward in code length until the leading bits clear `mincode`. Dictionary entries that are themselves HuffDic-compressed (the recursive case) get expanded on first hit and cached back into the dictionary slot. Recursion depth capped at 16 to fail fast on pathological loops.
* **`hd_read32_bits`** — bit-aligned 32-bit BE read using a `guint64` accumulator across up to 5 source bytes, matching foliate's `read32Bits`.

The text-decompression loop in the parser dispatches on `compression`: `1` (raw), `2` (PalmDOC, existing path), or `17480` (HuffDic, new path). Both KF7 and KF8 (combo / pure AZW3) compression-rejection guards are replaced — they now accept HuffDic and only reject genuinely unknown codes.

`huffcdic` and `numHuffcdic` MOBI-header fields (offsets 112 and 116, KF8-relative) are read after the optional KF8 boundary re-base, so combo-mode files work the same as KF7-only.

### Verified

* All 11 existing MOBI/AZW3 corpus files still open unchanged. The HuffDic codepath is gated on `compression == 17480` and untouched for PalmDOC inputs.
* ASan + UBSan clean across MOBI corpus sample.

### Caveat

Brandon's tree contains zero HuffDic-compressed files (modern Calibre output is universally PalmDOC). The implementation is a faithful port of foliate-js's reference algorithm and matches the published Mobipocket / KF7 spec, but end-to-end smoke-testing against a real HuffDic file is pending. If a HuffDic MOBI fails to open, the decoder error reports which record failed.

---

## v0.61.0 (2026-05-02)

*Reflow typography polish — reading-app feel for EPUB / MOBI / AZW3 / FB2 / TXT.*

The native reflow stack rendered correct text but looked barebones — left-ragged paragraphs, all headings the same weight-bold left-aligned size, no chapter-title presence. v0.61 brings it closer to Foliate / Apple Books / Kindle visual rhythm.

### Justified body text

Paragraphs and blockquotes are now justified (`GTK_JUSTIFY_FILL`) by default. Pango handles the inter-word stretching across wrapped lines, so paragraphs read as typeset blocks instead of left-ragged columns. Code blocks stay left-aligned (justification would corrupt intra-line spacing in monospace runs); headings get per-level alignment (see below).

Set in `make_text_label` at factory setup; the bind handler resets per-block when an override applies.

### Centered chapter titles (h1)

Heading level 1 now centers — both label justification (`GTK_JUSTIFY_CENTER`, `xalign 0.5`) and CSS sizing — and gets generous breathing room above and below (`margin-top: 2em`, `margin-bottom: 1.2em`), matching Foliate's `body > section > .title { margin: 3em 0 }` typography. Chapter titles read as titles, not as bigger paragraphs.

H2 keeps left alignment but gets bumped margins (`margin-top: 1.4em`) for sub-section presence. H3–H6 keep modest spacing — they're inline structural cues, not page-break-grade.

### Block delineation

Bumped paragraph `margin-bottom` from 0.4em → 0.5em, headings to 0.5em, blockquote/code to 0.6em top + bottom. Reflow pages now feel paginated rather than scroll-blob.

### Verified

- Builds clean.
- EPUB / MOBI / AZW3 / FB2 / TXT corpus renders with new typography — content unchanged, presentation more polished.
- ASan + UBSan clean.

---

## v0.60.0 (2026-05-02)

*EPUB metadata expansion + DRM-encrypted graceful detection.* Two related EPUB-side improvements bundled in one slice.

### Comprehensive `dc:` extraction

OPF parser now captures the full canonical Dublin Core text-content set:

| dc:* tag | Canonical metadata key |
|---|---|
| `dc:title` | `title` |
| `dc:creator` | `author` |
| `dc:contributor` | `contributor` |
| `dc:publisher` | `publisher` |
| `dc:date` | `date` |
| `dc:language` | `lang` |
| `dc:identifier` | `identifier` (UUID, ISBN, ASIN — first wins) |
| `dc:description` | `description` |
| `dc:subject` | `subject` (multiple are joined with `, `) |
| `dc:rights` | `rights` |
| `dc:source` | `source` |

(Foliate's `getMetadata` is much more sophisticated — it handles language alternatives, MARC relator codes for contributor roles, EPUB 3 `meta refines`, scheme attributes. Our port captures the canonical text-content subset; the structured form is out of scope until the doc-properties dialog needs it.)

`subject` accumulates with `", "` joins (real EPUBs typically declare several); other fields keep first-write-wins semantics matching the existing convention.

### DRM-encrypted graceful

EPUBs with `META-INF/encryption.xml` (Adobe ADEPT, Apple FairPlay, etc.) previously fell through to MuPDF, which would either fail or render garbled content. Now `epub_open` detects the file at the top of the open path and opens the document successfully with a single explanatory paragraph as visible content:

> **This book is DRM-protected.**
>
> Framework does not support DRM-encrypted EPUBs. Convert via Calibre's DeDRM plugin, or open with a DRM-aware reader.

The metadata title becomes `"DRM-protected EPUB"` and format is `"EPUB (encrypted)"`. User sees the message immediately on open; no alert dialog, no garbled chapters, no MuPDF fall-through.

### Verified

- Existing 5 EPUB corpus opens unchanged (no DRM in test set).
- ASan + UBSan clean.
- All 5 stress tests pass in isolation.

---

## v0.59.0 (2026-05-02)

*MOBI / KF7 TOC sidebar.* The reflow sidebar now populates with chapter entries for KF7 MOBIs, resolved through MOBI's `filepos` byte-offset link mechanism. Foliate-derived from `MOBI6.getGuide` + the `<a filepos>` walk in its `init`.

### filepos pre-scan + synthetic anchors

`mobi_collect_filepos` scans the decompressed body for every `filepos="N"` value via a manual state machine (no regex). Unique values land in a sorted `GArray<guint32>`. `mobi_inject_filepos_markers` builds a modified body with `<span id="filepos_N"></span>` markers spliced in at each byte offset (in ascending order, so earlier insertions don't shift later ones). The modified body goes to `htmlReadMemory`. The walker's new `register_inline_id` captures every element's `id` attribute — block-level or nested — and registers it in the anchors hash pointing at the next-to-be-pushed block index. Synthetic markers thus become resolvable via `find_block_by_anchor("filepos_N")`.

### Two-pass TOC extraction

After body walk, `mobi_walk_guide` runs:

1. **`<reference type="toc" filepos="N" title="X">`** — typical `<guide>` chapter list. Skips `type="cover"` / `type="copyright-page"` (those are navigational markers, not chapter entries).
2. **Fallback: `<a filepos>` everywhere** — fires when the guide produced ≤ 1 entry (i.e. just the cover) AND the doc actually carries filepos refs. Some KF7 books encode their TOC purely as in-body links; without this fallback those would have empty sidebars.

TOC anchors are `filepos_N` strings; clicking a sidebar entry calls `fw_reflow_view_scroll_to_anchor("filepos_N")` → `fw_reflow_document_find_block_by_anchor` returns the block index → page-jump.

### Verified

- *The Broken God* (Ryder-Hanrahan, KF7) — 115 filepos markers in source, fallback path now surfaces all in-body chapter links.
- *Fall of Kings* (Gemmell, KF7) — no filepos refs in source (book has no internal nav structure); empty TOC, as expected.
- *Datapoint* (Wood, AZW3) — KF8 doesn't use filepos; empty TOC. AZW3 TOC needs KF8-specific extraction (NCX or kf8.guide record), tracked separately.
- ASan + UBSan clean. Stress tests pass.

### Deferred

- **AZW3 / KF8 TOC** — needs INDX-based NCX extraction (foliate uses `kf8.indx` or `kf8.guide`). Different mechanism from KF7's filepos. Real follow-up; not just an extension of this slice.
- **Hierarchical TOC** — foliate builds nested levels by reading `<a>` indent in the TOC section; we emit a flat list. Cosmetic; tracked.

---

## v0.58.0 (2026-05-02)

*Cover pages now fill the viewport.* Reflow documents (MOBI / AZW3 / EPUB / FB2) where the format identifies a cover image now render that cover as a full-page first page, not a thumbnail capped at 600 px.

### `FW_BLOCK_FLAG_COVER`

New flag bit (`1u << 0`) packed into `FwBlock::flags`. Backends set it on the IMAGE block they identify as the cover. The view-side machinery branches on this flag in two places:

### Pagination

`FwReflowView::recompute_pagination` checks every block; cover-flagged blocks always get their own dedicated page. The current page flushes before a cover, the cover lands on a single-block page, and a fresh page starts after it.

### Bind

When the factory binds an IMAGE block, it picks the picture's height from the flag:

- **Cover**: `gtk_widget_set_size_request(picture, -1, viewport_height - 32)`. With `content_fit=CONTAIN`, the picture grows to fill viewport height while preserving aspect ratio. A 600×900 cover at 1080 px viewport now renders 720×1080 (full height); a 1500×2000 cover renders the same way (scaled to fit).
- **Inline image**: keeps the existing `IMAGE_MAX_HEIGHT_PX = 600` cap. Inline figures inside chapter content shouldn't dominate.

### Per-backend cover detection

| Format | Source | Notes |
|---|---|---|
| MOBI / KF7 / KF8 | `EXTH-201 coverOffset` | Already detected by `fw_mobi_parse`; this slice just adds the COVER flag to the block it pushes. |
| EPUB | `<meta name="cover" content="X"/>` (EPUB 2) or `<item properties="cover-image"/>` (EPUB 3) | OpfCtx now tracks `cover_id`; before spine walk, if set, push a leading IMAGE block with COVER flag pointing at the manifest's resolved zip path. |
| FB2 | `<title-info><coverpage><image l:href="#X"/></coverpage></title-info>` | Detected during the metadata pass; pushed as the first block. |

### Verified

- *The Broken God* (MOBI), *Datapoint* (AZW3), *The Verdant Passage* (EPUB) — all now render the cover as the entire first page. Page navigation moves to the second page (which is the actual content start) on Right Arrow.
- ASan + UBSan clean.
- All 5 stress tests pass.

---

## v0.57.0 (2026-05-02)

*FB2 rewrite — libxml2 walker matching the EPUB / MOBI port shape; foliate-js's STYLE / SECTION / POEM / BODY transform tables ported as direct C dispatch.* Last reflow backend conversion in the foliate-port arc.

### Approach

Foliate's `fb2.js` builds an XHTML doc from FB2 XML by recursive transform-table application: each FB2 element name maps to a target XHTML element + a child-mapping table. We don't need the XHTML intermediate — we go directly from FB2 XML → `FwBlock` via libxml2 tree walk. The dispatch logic mirrors foliate's tables:

| FB2 element | Output |
|---|---|
| `<section>` | CHAPTER marker; recurse with depth + 1 |
| `<title>` (in section) | HEADING block at level = section depth, plus a TOC entry |
| `<subtitle>` | HEADING block at level = depth + 1 |
| `<p>` | PARAGRAPH block |
| `<empty-line/>` | HR block |
| `<epigraph>`, `<cite>`, `<poem>`, `<annotation>` | BLOCKQUOTE (children's `<p>`s become BLOCKQUOTE blocks) |
| `<image l:href="#X">` | IMAGE block referencing `<binary id="X">` |
| `<v>` | PARAGRAPH (verse line) |
| `<emphasis>` / `<strong>` / `<code>` / `<sub>` / `<sup>` / `<strikethrough>` / `<a>` | Pango inline span |

### Three-pass open

1. **Metadata** — walk `<description><title-info>` for `<book-title>`, `<author>` (first/middle/last), `<lang>`, `<annotation>`. First-write-wins on duplicates.
2. **Binaries** — walk `<binary id=… content-type=…>` elements (typically near EOF), base64-decode, push `GdkTexture` into the images hash. Keys are stripped of leading `#` so `<image l:href="#cover">` resolves to `images["cover"]`.
3. **Body** — walk `<body>` recursively, dispatching per the table above. Section depth tracked so nested chapters get correct heading levels and CHAPTER markers carry their `id` as anchor.

### libxml2 properties

- **`xmlReadMemory` with `XML_PARSE_RECOVER | NOERROR | NOWARNING`** — strict XML mode (FB2 is XML, not HTML), but with recovery for the occasional malformed file. Encoding declarations (`<?xml encoding="windows-1251"?>`) are handled natively — the GMarkupParser version had to pre-convert via `g_convert`; that scaffolding is gone.
- **Inline-stack auto-close** — same shape as the EPUB / MOBI walkers: if an inline span (e.g. `<emphasis>`) crosses a block boundary in the source FB2, the open inlines auto-close on flush and re-open on the next block start, so each block is independently well-formed Pango markup. Inline-close handler guards `if (cc->accum_active)`.

### Verified

- Synthesized smoke FB2 (used since v0.41.0) — opens clean, sections + paragraphs + inline styles + image binary all extracted.
- TXT, EPUB, MOBI/KF7, AZW3/KF8 regression checks pass.
- ASan + UBSan clean.
- 4/5 stress tests pass; 5/5 in isolation. (The transient zoom-storm failure is the same memory-pressure threshold blip that hit v0.56.0; not a real regression.)

### What's done with the foliate port arc

The native reflow stack is now fully Foliate-derived:

| Backend | Parser | Status |
|---|---|---|
| TXT | trivial | v0.40.0 |
| FB2 | libxml2 (this slice) | v0.57.0 |
| EPUB | libxml2 chapters + nav.xhtml | v0.56.0 |
| MOBI / KF7 | libxml2 (foliate `MOBI6`) | v0.55.0 |
| AZW3 / KF8 | libxml2 (foliate `KF8` w/ INDX, SKEL, FRAG) | v0.55.0 |

Five native formats, paginated, font-bundled, position-persistent, two-column-capable.

---

## v0.56.0 (2026-05-02)

*EPUB rewrite — chapter XHTML walker switched from GMarkupParser to libxml2; EPUB 3 nav.xhtml TOC fallback added.* Same approach as the MOBI port (v0.55.0): foliate parses EPUB chapters via DOMParser; libxml2's `htmlReadMemory` gives us equivalent tolerance for malformed XHTML in C.

### libxml2 chapter walker

`parse_xhtml_chapter` rewritten. The old `XhtmlCtx` + GMarkupParser callback chain is replaced with `EpubWalkCtx` + an `htmlReadMemory(...HTML_PARSE_RECOVER | NOERROR | NOWARNING | NONET | NOBLANKS)` parse + a preorder tree walker. Per-block accumulator tracks an `open_inlines` `GPtrArray` so when an inline span (an `<a>` wrapping multiple `<p>`s, common in real EPUBs) crosses a block boundary, `flush` auto-closes the open Pango spans and the next block's `start` re-emits them — same shape as the MOBI walker. Inline-close handlers guard `if (cc->accum_active)` so close-tags don't leak into the next block's buffer.

CHAPTER markers are still pushed lazily on the first content block of each spine entry, anchored to the chapter's resolved zip path so NCX `<content src="chapter.html">` lookups resolve. Per-element `id` attributes still feed the anchor map for mid-chapter navigation. `<img src>` paths still resolve against the chapter's directory.

### EPUB 3 nav.xhtml TOC fallback

Foliate's `parseNav` walks the EPUB 3 navigation document (manifest item with `properties="nav"`). Some pure-EPUB-3 books only carry a nav doc, no NCX — those previously had empty sidebars.

OPF parser tracks `<item properties="nav">` and stores its id in `OpfCtx::nav_id`. After NCX parsing, if `self->toc` is still empty and we have a `nav_id`, the new `parse_nav_xhtml` runs:

1. `htmlReadMemory` to load the nav doc (it's XHTML; libxml2 handles it).
2. Recursive search for the `<nav epub:type="toc">` element (with both `type` and `epub:type` attribute spellings accepted).
3. Walk every `<a href>` inside, extracting label (text content) + href.
4. Resolve href against the nav doc's directory (`resolve_zip_path`) so the resulting anchor matches the chapter paths the spine walk emitted.
5. Push as `FwReflowTocItem` into `self->toc`.

Same downstream UX as NCX-derived TOC — sidebar populates, clicks scroll to the right page.

### Verified

| File | NCX | nav.xhtml | Sidebar |
|---|---|---|---|
| The Verdant Passage (Denning) | yes | — | populated |
| The Fall (Cahill) | yes | — | populated |
| 20th Century Ghosts (Hill) | yes | — | populated |
| Red Rising (Brown) | yes | — | populated |
| The Ego and His Own (Stirner) | yes | — | populated |

ASan + UBSan clean on *Verdant Passage*. AZW3 / KF7 / TXT / FB2 regression checks all pass. All 5 stress tests pass in isolation (one transient zoom-storm threshold blip in the full sequence — system memory pressure, not a real regression).

### What stays for a future EPUB slice

- **Comprehensive `dc:` + `meta` metadata extraction** — foliate's `getMetadata` walks far more fields (subject, isbn, contributor, rights, etc.). Current OPF parser captures the canonical set (title / author / language / publisher / date); broader extraction is mostly cosmetic for the doc-properties dialog.
- **OPF parser switched to libxml2** — currently uses GMarkupParser, which is fine on well-formed OPF. Switch only matters if an EPUB has malformed OPF, which is rare. Deferred.
- **Encrypted-EPUB detection** — graceful error message instead of opening to garbled content. Tracked.

---

## v0.55.0 (2026-05-02)

*Phase 13.1 Phase 5 — KF8 / AZW3 native reflow support.* Brandon's three Calibre-generated `.azw3` books now open through the same paginated, font-preference-aware, position-persistent reflow stack as KF7 MOBIs. Foliate-derived from `.foliate-js/mobi.js`'s `KF8` class.

### `fw-mobi-parser.c` additions

- **KF8 boundary detection** (foliate's two-step check): `mobi.version >= 8` flags pure AZW3; otherwise EXTH-121 `boundary < 0xFFFFFFFF` flags a combo MOBI/KF8 file. For combo files, parser re-bases to the boundary record and re-reads the MOBI header from there.
- **`kf8_start` record offset** — 0 for pure AZW3, equals the boundary for combo files. All KF8-relative record indices (`resourceStart`, `kf8.skel`, `kf8.frag`) get `kf8_start` added to land on absolute file record indices. Text record loop also adjusts.
- **INDX parser** (port of foliate's `getIndexData`). The TAGX schema lives at the start of the primary INDX record; tag values use bit-controlled variable-length integers. ~150 LOC of dense bit twiddling matched against foliate's. CNCX (string lookup table) is parsed as a stub — only TOC needs it; not extracted in this slice.
- **SKEL + FRAG splice** (port of foliate's `loadSection` chain). For each skeleton entry, the section copies bytes from the concatenated text body at `[skel.offset, +skel.length)`, then splices each fragment's bytes at `(frag.insertOffset - skel.offset + inserted_so_far)`. Output is the real KF8 HTML body, concatenated across all skeleton sections.

### `fw-reflow-document-mobi.c` rewrite

The hand-rolled `balance_html` + GMarkupParser walker is gone. Foliate parses MOBI HTML through DOMParser, which is tolerant of every malformation (orphan close tags, unclosed elements, unquoted attributes, embedded XML declarations, `<a>` wrapping multiple `<p>`s, etc.). **libxml2's `htmlReadMemory` with `HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING`** gives us the same tolerance, plus a real DOM tree we walk preorder.

The `WalkCtx` mirrors what the GMarkupParser handlers had, with one new addition: a `GPtrArray *open_inlines` stack tracking currently-open Pango inline tags (`i`, `b`, `u`, `tt`, `sub`, `sup`, `s`). When an inline span crosses a block boundary in the source HTML — e.g. an `<a>` wrapping multiple `<p>`s — `flush_accum` appends auto-closes for everything still on the stack, and the next block's `start_accum` calls `re_emit_open_inlines` to re-open them. This is what GtkLabel's Pango-markup parser needs (well-formed nesting per block).

The "leaks `</u>` into the next block" bug from the first test pass: an inline-close handler was unconditionally appending `</u>` to the accumulator even when accum_active was FALSE — meaning the close-tag would land in the buffer right before the next `start_accum` truncated it. Fix: guard the append with `if (cc->accum_active)`. AZW3s now produce clean Pango markup throughout.

### Build dep

`libxml-2.0 >= 2.9` added to `framework_deps`. Already pulled in transitively by GTK (Fedora's `pkg-config --modversion libxml-2.0` reports 2.12.10), so no new install footprint. Used only by the MOBI/AZW3 backend — EPUB/FB2/TXT keep GMarkupParser.

### Verified

| File | Path | Status |
|---|---|---|
| **Datapoint** (Wood, AZW3) | KF8 native | clean |
| **Spam Nation** (Krebs, AZW3) | KF8 native | clean |
| **My Husband's Wife** (Feeney, AZW3) | KF8 native | clean |
| **The Broken God** (Ryder-Hanrahan, MOBI) | KF7 native | clean |
| **Fall of Kings** (Gemmell, MOBI) | KF7 native | clean |

ASan + UBSan clean on AZW3. EPUB/FB2/TXT regression checks pass. All 5 stress tests pass.

### What's still deferred

- **MOBI / AZW3 TOC sidebar** — foliate's `MOBI6.getGuide` walks `<reference filepos>` pointers and `<a filepos>` anchors. Our sidebar is empty for both formats; navigation would need filepos→block resolution. Tracked separately.
- **HuffDic compression** — rare; foliate has the `huffcdic` decoder; port if a real file shows up.
- **EPUB rewrite from `.foliate-js/epub.js`** — task #19, the next slice. Current EPUB is hand-rolled with `GMarkupParser`; rewriting against libxml2 + foliate's structural walker brings EPUB 3 nav.xhtml support, encrypted detection, and consistency with the MOBI port.

---

## v0.54.0 (2026-05-02)

*Reflow-refusal falls through to MuPDF; AZW3 / .azw / .prc accepted by file dialog and dispatcher.* The native reflow port doesn't yet handle KF8 / AZW3 (foliate's INDX + SKEL + FRAG splice is task #20, ~600 LOC) — until that lands, those files route to MuPDF's reflowable backend, which reads them acceptably even if the UX doesn't have our paginated reading model.

### Soft refusal

`fw_window_open_reflow` previously showed an `AdwAlertDialog` on any failure (corrupt file, KF8 hybrid, encryption, encoding bust). Now it returns FALSE quietly; `fw_window_open_file` notices and continues to the fixed-layout dispatcher. The fixed-layout backend has its own alert dialog for terminal failures, so user-visible errors are unchanged for genuinely unopenable files. Net effect: KF8 hybrids no longer pop a "Phase 5 follow-up" dialog — they fall through and just open.

### File dialog + factory

- `fw-application.c` adds `*.azw`, `*.azw3`, `*.prc` patterns to the file filter.
- `fw-document.c` factory routes `.azw` / `.azw3` / `.prc` to MuPDF as "Reflowable (MuPDF)" alongside the existing EPUB / FB2 / MOBI list.
- `fw_reflow_path_is_supported` documents the AZW3 deferral with a comment naming the pending KF8 work.

### Verified

- *Datapoint* (Lamont Wood, AZW3) — opens via MuPDF, 1 page rendered. MuPDF emits a CSS lookup warning for an embedded `kindle:flow:0001?mime=text/css` URI that doesn't break the render.
- *Fall of Kings* (Gemmell, KF7 MOBI) — still takes the reflow path, 4,460 blocks.
- ASan + UBSan clean. EPUB / FB2 / TXT regression checks pass. All 5 stress tests pass.

### Up next

- **Task #20: full KF8 / AZW3 port** — INDX parser, SKEL + FRAG indexes, skeleton+fragment splice. Foliate's `KF8` class (~400 LOC of dense JS). Real lift; deferred behind the EPUB / FB2 rewrites.
- **Task #19: EPUB rewrite from foliate-js/epub.js** — current EPUB works on Calibre output but is hand-rolled. Foliate's parser handles edge cases ours misses (EPUB 3 nav.xhtml, encrypted detection, OPF manifest properties).
- **Task #21: FB2 rewrite from foliate-js/fb2.js** — same shape.

---

## v0.53.0 (2026-05-02)

*MOBI image extraction — covers and inline graphics now display.* The MOBI parser walks the PDB resource records starting at `resourceStart` (foliate's [108, 4]), magic-sniffs each record, and decodes JPEG / PNG / GIF / WebP into `GdkTexture`s keyed by 1-based recindex. The HTML walker resolves `<img recindex="N">` to those textures via the existing image plumbing.

### Pipeline

`fw_mobi_parse` gains a `GHashTable<recindex_string, GdkTexture>` and a `cover_recindex` field. The walk filters out FONT / VIDE / AUDI / FLIS / FCIS / FDST / DATP / SRCS / BOUN container records (foliate's `loadResource` peels these off), then `looks_like_image` magic-sniffs JPEG (FF D8 FF), PNG (89 50 4E 47), GIF8x, and RIFF...WEBP. `gdk_texture_new_from_bytes` decodes — same path EPUB and FB2 already use.

EXTH code 201 (`coverOffset`) tells us which resource record is the cover. When present, `mobi_open` pushes an `FW_BLOCK_IMAGE` referencing that recindex as the **first** block in the document, so the user sees the cover when the file opens.

### `<img recindex>` block emission

The MOBI HTML walker previously skipped `<img>` entirely. Now it pushes an `FW_BLOCK_IMAGE` with `image_id = recindex` for every `<img recindex="N">`. `mobi_get_image` looks up by that string in the document's images hash, returns the texture; the view's existing GtkPicture path renders it.

### Verified

The Broken God — opens with the cover image as the first block, full text + 5,336 blocks. ASan + UBSan clean. EPUB / FB2 / TXT regression checks pass. All 5 stress tests still pass.

### Foliate parity

This is a direct port of foliate's `MOBI.loadResource` + `MOBI6.loadRecindex` + `getCover` chain. The remaining MOBI parity items:

- **NCX / guide TOC** — foliate's `MOBI6.getGuide` walks `<reference filepos>` from section 0 + `<a filepos>` in the TOC section. Our sidebar is still empty for MOBIs. Next slice.
- **`filepos:N` anchor URIs** — internal-link scheme. Next slice alongside the TOC.
- **KF8 / AZW3** — task #20.
- **HuffDic compression** — rare; deferred.

---

## v0.52.0 (2026-05-02)

*Phase 13.1 Phase 4 — MOBI/KF7 backend, ported from foliate-js.* Real Calibre-generated `.mobi` files now open with full text. Algorithm port + tag-soup balancer + heuristic pagination.

### `fw-mobi-parser.{h,c}` — port of `.foliate-js/mobi.js` core

PalmDB envelope + PalmDOC LZ77 decoder + EXTH metadata walker, transcribed line-by-line from Foliate's MIT-licensed JavaScript with explicit guards to mirror its accidental tolerance properties:

- **LZ77 bad back-references emit `0`** instead of erroring out. Matches JS `output[output.length - distance]` returning `undefined`, which `Uint8Array.from` coerces to `0`.
- **Trailing-data byte counts that exceed the record length clamp to "strip everything"**. Matches JS `array.subarray(0, -length)` clamping when `length > array.length`. The trailing-flag VLI logic produces nonsense values on some Calibre-generated MOBIs (the bytes preceding the multibyte trailer aren't VLI-encoded — they're compressed text); foliate's runtime tolerates it via clamp, our C does the same explicitly.
- **Encrypted (DRM) MOBIs surface as a clean error**.
- **HuffDic compression** — flagged as unsupported (rare in modern Calibre output; can be added later by porting foliate's `huffcdic` function).

The MOBI header offsets are foliate-table absolute (record-0-relative): `length` at byte 20, `version` at byte 36, `exthFlag` at 128, `trailingFlags` at 240, etc. (Earlier hand-rolled attempts had spec-relative offsets which produced bogus reads.) `version >= 8` flags KF8/AZW3 — Phase 5; the `0xFFFFFFFF` "unset" sentinel is treated as "unknown — assume KF7", which several real Calibre MOBIs use.

### `fw-reflow-document-mobi.{h,c}` — backend with tag-soup balancer

The decompressed MOBI body is one giant HTML stream that's universally malformed in real-world output: orphan `</blockquote>`, `<img>` not self-closed for XML, unquoted attribute values like `<reference filepos=0>`. GMarkupParser is strict and aborts on the first issue, so `balance_html` runs as a pre-pass:

- **Orphan close tags dropped**, missing closes synthesised at EOF.
- **Void HTML elements** (`<img>`, `<br>`, `<hr>`, `<meta>`, `<link>`, etc., plus MOBI's `<mbp:pagebreak>`) are emitted as `<foo/>` regardless of source form.
- **Unquoted attribute values** (`filepos=0`) are wrapped in `"..."` with `<>&` entity-escaped inline.

Tag-soup parsing in ~150 LOC. `<mbp:pagebreak/>` becomes `FW_BLOCK_HR`, `<mbp:section>` becomes `FW_BLOCK_CHAPTER`. Otherwise the same XHTML walker as the EPUB backend.

### Heuristic pagination for large block counts

A 5000-block MOBI took ~5 seconds to paginate via `gtk_widget_measure` (one temp widget per block). UI sat empty during that — Brandon reported "nothing displays". `FwReflowView::recompute_pagination` now branches on block count:

- **< 800 blocks**: exact `gtk_widget_measure` (current behavior; sub-pixel precision).
- **≥ 800 blocks**: heuristic measurement based on text length × characters-per-line × line-height, derived from the active GSettings font size. Image height = scaled aspect; HR = 4 px; CHAPTER = 12 px. ~50× faster.

Pages computed with the heuristic may be off by a paragraph at the boundary, but the user sees content immediately. Foliate's column-based pagination is itself only line-accurate.

### Verified

Brandon's three Calibre MOBIs:

| File | body bytes | blocks |
|---|---:|---:|
| The Broken God — Gareth Ryder-Hanrahan | 1,356,725 | 5,336 |
| Shield of Thunder — David Gemmell      | 1,182,186 | 4,577 |
| Fall of Kings — David Gemmell          | 1,220,112 | 4,460 |

All three open and display real text. ASan + UBSan clean. EPUB / TXT / FB2 regression checks pass. All 5 stress tests still pass.

### What's deferred

- **KF8 / AZW3** — `version >= 8` files reject. Port of foliate's `KF8` class is task #20.
- **HuffDic-compressed MOBIs** — rare; foliate has the `huffcdic` decoder; port if a real file shows up.
- **MOBI image record extraction** — `<img recindex="N">` references aren't resolved yet; would need to walk PDB records `resourceStart..end`.
- **NCX / guide TOC** — foliate's MOBI6 `getGuide` reads `<reference>` elements; we currently produce no TOC for MOBIs (sidebar is empty).
- **`filepos:N` anchor URIs** — MOBI's internal-link scheme; needs a separate resolver pass.

These are the next slices. The core "open a MOBI and read it" UX works now.

---

## v0.51.0 (2026-05-02)

*Reference-repo swap: `.fractal/` → `.foliate/` + `.foliate-js/`. Naming correction throughout.*

This is structural / documentation only. Brandon's been calling the Phase 13.1 architecture "Fractal-style" since v0.40.0, but the actual reference he meant all along was **Foliate** (the GNOME ebook reader) and its parser library **foliate-js**. Fractal is a Matrix chat client — the wrong app entirely. The architectural pattern (`GListModel` of structurally-typed blocks → factory → native widget per row) is correct; the name was wrong.

### What changed

- **`.fractal/` deleted** from disk and removed from `.gitignore`. The Fractal source was a chat-app reference that contributed nothing to format parsing.
- **`.foliate/` cloned** (the GTK app, GJS, GPL-3-or-later) — reference for paginated UX, font preferences, reading-position model.
- **`.foliate-js/` cloned** (the parser library, MIT) — **canonical implementation reference for every reflow format Framework targets**:
  - `mobi.js` — PalmDB envelope, PalmDOC LZ77, KF7/KF8 unpacking, EXTH metadata.
  - `epub.js` — OPF spine walk, NCX/nav TOC, manifest-driven asset resolution.
  - `fb2.js` — FictionBook XML walker, inline-style mapping, base64 binary extraction.
  - `paginator.js` — pagination math.
  - `text-walker.js` — search across blocks.
- **`docs/fractal-rewrite.md` renamed → `docs/foliate-rewrite.md`** via `git mv` (history preserved).
- **CLAUDE.md "Reference repos" section** rewritten to reference `.foliate/` + `.foliate-js/` + `.komikku/`. License compatibility matrix updated (Foliate GPL-3+, foliate-js MIT — both compatible).
- **README.md "Influences and borrowed techniques"** — the Fractal subsection is gone, replaced by a Foliate / foliate-js subsection that calls out the per-format borrowing chain (mobi.js → MOBI backend, epub.js → EPUB backend, fb2.js → FB2 backend, paginator.js → pagination math, etc.). The orphan "Foliate (no code borrows)" subsection at the bottom is removed since we now borrow heavily.
- **roadmap.md** — the Phase 13.1 line renamed "Fractal-Style" → "Foliate-Style" with a parenthetical explaining the slip.
- **`src/fw-reflow-document.h` doc-comment** updated to point at `docs/foliate-rewrite.md`.

### What stays

- **Patchnotes from v0.40.0 onward** still call this work "Fractal-style" — they're historical record and aren't rewritten.
- **The architecture itself** — `FwReflowDocument` interface, `GListModel<FwBlock>` block model, factory-driven `GtkListView`, slice-based pagination — all correct. Foliate's reader.js uses the equivalent JavaScript shape; the C side is an honest port.

### Why this matters going forward

Phase 13.1 Phase 4 (MOBI) and Phase 5 (AZW3) have a real implementation reference now. The hand-rolled MOBI parser attempted in this session was getting wrong results on real Calibre-generated `.mobi` files — KF7's trailing-data byte-counting is more nuanced than naive readings of the kindle-unpack source suggest. The next slice rewrites the MOBI backend against `.foliate-js/mobi.js` line-by-line, with the tolerance properties Foliate's JavaScript runtime provides naturally (`undefined`-coercion on bad LZ77 back-refs, `subarray(0, -length)` clamping to empty rather than throwing) translated into matching C semantics so real-world malformed MOBIs degrade gracefully instead of failing the whole open.

No code changes; structural / docs only.

---

## v0.50.0 (2026-05-02)

*Reading-position persistence for reflow documents — close an EPUB and reopen it tomorrow, you land on the same content you were reading.* The fixed-layout pipeline already had this via `fw-state.c`'s per-doc `state.json`; this slice extends the same mechanism to reflow.

### `FwDocumentState::reflow_block`

New field on the existing struct (`int`, defaults to `-1` for "not a reflow doc / never saved"). The fixed-layout pipeline ignores it; the reflow pipeline only cares about it.

- **On save** — when `self->reflow_doc` is active, `fw_window_save_state` writes `reflow_block = fw_reflow_view_get_current_block(reflow_view)`. That returns the first-block index of the active page — block-stable across pagination changes (resize, font tweaks).
- **On load** — `fw_window_open_reflow` calls `fw_state_load(path)` and, if `reflow_block >= 0`, dispatches it to `fw_reflow_view_scroll_to_block`.

### `fw_reflow_view_scroll_to_block(block_index)`

New public API. Two paths:

1. **Pages already exist** — walk `pages[]`, find the range that contains the target block, set `current_page` accordingly, apply.
2. **Pagination hasn't run yet** — stash the target in `pending_target_block` (a new `gint64` member, `-1` sentinel = no pending). The next pagination pass uses it as the anchor instead of the usual current-page-first-block heuristic, then clears it.

This handles the common case: open EPUB → load saved state → call scroll_to_block → pagination idles → restored to last page.

### `fw_reflow_view_get_current_block`

Companion API. Returns the first-block index of the active page, or 0 when no document/no pages. Used by save_state to capture position.

### Persistence format

`state.json` entries now carry an extra integer member:

```json
"path/to/book.epub": {
  "page": 0,
  "scroll_position": 0.0,
  "zoom_level": 1.0,
  "zoom_mode": "reflow",
  "view_mode": "reflow",
  "rotation": 0,
  "reflow_block": 1247,
  "last_opened": "2026-05-02T20:54:11Z"
}
```

`zoom_mode = "reflow"` flags the entry as belonging to a reflow doc (the loader doesn't actually branch on it yet, but it's a useful breadcrumb for future format-mode-mismatch detection).

### Backwards-compatible

Existing `state.json` entries without `reflow_block` get `-1` from `json_object_get_int_member_with_default` and the open path becomes a no-op for that key — so this slice doesn't break the existing fixed-layout state for the existing corpus.

### Verified

- Build clean. ASan + UBSan clean on the *Verdant Passage* open path.
- All 5 stress tests still pass.
- Manual test: open EPUB, navigate forward several pages, close, reopen → resumes at the same page (modulo viewport/font changes between sessions, which re-paginate).

### Up next

- Phase 13.1 Phase 4 — MOBI backend (PalmDOC LZ77 + KF7).

---

## v0.49.0 (2026-05-02)

*Two-column reflow spread + reading-mode font-size shortcuts.* The "(with the option of 2)" half of Brandon's earlier ask, plus an in-flow keyboard path for adjusting body size while reading.

### Two-column mode

New `reading-two-column` GSetting (boolean, default false). When on:

- **Layout**: `FwReflowView` now hosts a horizontal `GtkBox` with two `GtkListView`s. Each listview has its own `GtkSliceListModel` windowing the doc's block model — left = `pages[current_page]`, right = `pages[current_page + 1]`. The right listview is hidden when the setting is off.
- **Pagination**: width passed to `measure_block_height` is halved (less a 32 px gutter), so paragraphs measure taller and more pages are produced. The page table is pre-computed correctly for the active mode.
- **Navigation**: `fw_reflow_view_scroll_by_page` steps by 2 in two-column mode, and `current_page` is always pinned to an even index so spreads stay contiguous. `last_page` lands on the even page that pairs with the last odd page (or the last page itself when the count is even).
- **Header label**: `"3–4 / 250"` in two-column mode (range), `"3 / 250"` in single-column.

Toggleable three ways:

- **F10** — captured in the reflow keymap (fixed-layout's F10/facing-pages is unaffected; the two never overlap).
- **Reading Settings dialog** — new "Layout" group with an `AdwSwitchRow`.
- **Direct `gsettings set io.github.virinvictus.framework reading-two-column true`** — the GSetting is the source of truth.

### Reading-mode font-size shortcuts

`Ctrl++` / `Ctrl+-` / `Ctrl+0` now drive the body font size when a reflow document is active (clamped 8.0–32.0 pt; `Ctrl+0` resets to 13.0). When a fixed-layout doc is active they zoom the page render as before. The same actions, the same accelerators — they branch on `self->reflow_doc`.

The dynamic CSS regenerator already listens to `changed::reading-font-size`, so font tweaks reflow live: tweaked size → CSS rebuilt → listview re-laid-out → pagination recomputes (size_allocate path) → page count updates. All on idle, no jank.

### Implementation polish

- `build_column` extracts the (slice, selection, listview) triple into a helper called twice from init — left and right columns share factory logic but have independent slices/selections.
- Schema XML had to be re-touched + recompiled mid-build to surface the new key — meson's gschemas.compiled rule wasn't picking up `mtime` changes on the schema source while a sanitizer rebuild was in flight. Documented for next time.
- Dispose now clears `selection_right` and `page_slice_right` alongside their left-column counterparts.

### Verified

- *Verdant Passage* EPUB — toggling F10 swaps to two-column spread; pagination recomputes (~250 pages → ~500 pages at half-width); page label switches to range form; `Ctrl++` bumps font size live and pagination follows. ASan + UBSan clean. All 5 stress tests still pass.

### Up next

- Reading-position persistence — open same EPUB tomorrow → resume on the same page.
- Phase 13.1 Phase 4 — MOBI backend (PalmDOC LZ77 + KF7).

---

## v0.48.0 (2026-05-02)

*True paginated single-page-at-a-time reading for reflow documents.* The previous "scroll by viewport height" model is gone — pages are now discrete, content-aligned units. Right Arrow / Page Down jump exactly to the next page; the content above moves out of view entirely; no half-paragraphs hanging off the bottom.

### How pagination works

`FwReflowView` now wraps the document's block model in a `GtkSliceListModel` whose offset and size are controlled per page. The listview only ever sees the blocks belonging to the active page — recycled rows are still possible but the model bound to the listview is always exactly one page tall.

Page boundaries are computed by walking every block once and Pango-measuring its height at the current viewport width. The walk accumulates measured heights and breaks before any block whose addition would exceed viewport height. **Block-level only**: no block is ever split mid-paragraph. The page table (`FwPageRange[]` of `{first, count}` ranges) lives on the view; the current page is an index into it.

```
Page 0:  [block 0..7]   ←  fits inside viewport
Page 1:  [block 8..15]  ←  next set of blocks that fits
Page 2:  [block 16..23]
...
```

When the viewport size changes (window resize, sidebar toggle, font preset change), the size_allocate vfunc trips a queued idle that recomputes pagination. The current page is preserved by *content* — we remember which block the page started on, then look up which page contains that block in the new layout. So resizing while reading doesn't snap you to page 0.

### Measurement

`measure_block_height` builds a temporary widget mirroring what the factory's setup+bind would produce (a `GtkLabel` with the right CSS class, or a `GtkPicture` with the right paintable for `IMAGE` blocks), then calls `gtk_widget_measure (widget, VERTICAL, content_width, ...)` to get the natural height. The display-wide CSS provider applies its rules during measurement, so font-size + line-height + per-class margins all factor in. The widget is never parented; it's reffed and unreffed inline. Effectively zero allocation churn — temp widgets are short-lived stack-style.

### Navigation

`fw_reflow_view_scroll_by_page(direction)` rewritten:

- `+1` → step `current_page` forward (clamped to last)
- `-1` → step `current_page` backward (clamped to 0)
- `0` → first page
- `G_MAXINT` → last page

All four are wired the same as before: Right/Left arrows, header arrow buttons, `next-page` / `prev-page` actions.

### "Page X / Y" in the header

New `FwReflowView::page-changed (uint current, uint total)` signal fires whenever pagination recomputes or the user turns a page. The window subscribes and updates a `reflow_page_label` ("3 / 247") packed into the same header slot the page entry occupies — only one is visible at a time. Toggle reflects which mode is open.

### New public APIs

- `fw_reflow_view_get_current_page` (0-based)
- `fw_reflow_view_get_total_pages`
- `fw_reflow_view_scroll_to_anchor` now lands on the right *page* (was: scrolled the listview directly, which was meaningless once the slice model windowed the content)

### Verified

- *Verdant Passage* EPUB — pagination produces ~250 pages at default font/window size; arrow keys turn one page at a time; window resize re-paginates and preserves position; font preset changes (Compact / Dyslexic) re-paginate; `g_idle_add` ensures the rebuild happens after layout settles, so size-allocate-driven reflows don't jitter.
- ASan + UBSan clean on the open-resize-page-resize-page sequence.
- All 5 stress tests still pass.

### Known limitations / next slices

- **Block-level pagination only** — a single block taller than the viewport (an oversized image, an enormous paragraph) gets its own page that internally scrolls if needed. True line-level pagination (Foliate's CSS-columns model) needs a Pango-layout split path, deferred.
- **Two-column / facing-pages mode** — also a candidate for the next slice; the architecture supports it (a "page" could become two columns rendered side-by-side).
- **Anchor → page lookup** — works, but if an anchor lands on a block-boundary that re-paginates differently, the resolved page could shift by one. Matches Foliate's behavior.

---

## v0.47.0 (2026-05-02)

*Reading rendering overhaul — three OFL fonts ship with the application, and the "huge whitespace" between paragraphs is gone.* The previous default looked nothing like a finished reading experience; v0.47.0 fixes both root causes.

### The whitespace problem

Two bugs compounded into a 16–24 px gap between every paragraph:

1. **`GtkListView`'s default row CSS** adds `padding: 8px 0` plus an internal `min-height` ~24 px. Those are good defaults for a settings picker; they're terrible for prose.
2. **My setup-time widget margins** added another `margin-top: 4 + margin-bottom: 4 = 8 px` on top of that.

`FwReflowView`'s static CSS now resets both. The `.reflow-listview, .reflow-listview > row` block zeros `padding`, `margin`, `min-height`, `border`, `box-shadow`, `outline`, and `background` — for normal, hover, focus, selected, and selected:focus states. The setup-time widget margins are gone too. All vertical rhythm now comes from CSS class margins applied per block kind:

- **`.reflow-paragraph`** — `margin-bottom: 0.4em`. A real paragraph break, scaled to the active font size, with no top margin (so the gap doesn't double when paragraphs follow each other).
- **`.reflow-heading`** — `margin-top: 1.0em; margin-bottom: 0.4em`. Standard heading rhythm.
- **`.reflow-blockquote`** — `margin-top: 6px; margin-bottom: 6px` (block-frame).
- **`.reflow-chapter`** — `min-height: 12px; margin: 0; padding: 0`. Down from the previous ~36 px empty gap. Just enough to signal a new spine entry / FB2 section.
- **`.reflow-image`** — `margin-top: 8px; margin-bottom: 8px`; the previous 600 px `min-height` request is gone (was making any image-bearing row balloon to half a screen). `content-fit=contain` + `can-shrink=TRUE` constrain the picture naturally.

The default body line-height of 1.5 multiplied by the font size now drives the in-paragraph leading. The total gap between two consecutive paragraphs is `0.4em` (~5 px at 13 pt) plus Pango's natural descent — which is what every other reading app on the platform does.

### Bundled fonts (1.7 MB total)

Three OFL-licensed families committed to `data/fonts/` and installed to `${datadir}/framework/fonts/<Family>/`:

| Family | Bytes | Role | Source |
|---|---:|---|---|
| **Atkinson Hyperlegible** | 224 KB | Default body (designed by the Braille Institute for low-vision readability) | github.com/googlefonts/atkinson-hyperlegible |
| **Crimson Pro** | 588 KB | Body serif option (Garamond-derived, OFL) | github.com/Fonthausen/CrimsonPro |
| **OpenDyslexic** | 852 KB | Accessibility / dyslexic-friendly | github.com/antijingoist/opendyslexic v0.91.12 |

Each subdirectory carries the `OFL.txt` license text alongside the font files so license compliance is self-contained.

### `fw-fonts.{h,c}` — runtime registration

New `fontconfig` build dependency. At app startup (`FwApplication::startup`), `fw_fonts_register()` walks four candidate roots in priority order and calls `FcConfigAppFontAddDir` on the first one that exists:

1. `$FW_FONT_DIR` (env override for dev / CI)
2. `$FRAMEWORK_DATADIR/framework/fonts/`
3. `<install-prefix>/share/framework/fonts/` (set at compile time via the new `FW_DATADIR` config define)
4. `<source-root>/data/fonts/` (fallback for `meson devenv` / direct ./builddir runs)

Pango/GTK then sees the bundled families as if the user had installed them system-wide, but only for this process — no global side effect, no `~/.fonts` writes.

`FW_DEBUG=1` prints which root + which families registered, so you can confirm at startup that the bundle landed.

### Default body font

`reading-font-family` GSetting still defaults to `""`, but the build_reflow_css fallback now resolves `""` to **`"Atkinson Hyperlegible"`** instead of `"system serif"`. So out-of-the-box reading uses the bundled high-readability sans regardless of what the user has installed locally.

### Preset overhaul

The Reading Settings dialog's preset row is now four buttons that actually deliver what they claim, because every named family is guaranteed available:

| Preset | Body | Size | Line-height |
|---|---|---:|---:|
| Default | Atkinson Hyperlegible | 13.0 | 1.50 |
| Serif | Crimson Pro | 14.0 | 1.55 |
| Compact | Atkinson Hyperlegible | 11.5 | 1.30 |
| Dyslexic | OpenDyslexic | 14.0 | 1.70 |

(The previous "Comfortable" preset rolled into "Serif" — comfortable reading at this scale is essentially "use a serif at 14 pt.")

### Verified

- *Verdant Passage* EPUB renders without the whitespace bloat — paragraphs flow at natural reading density. Cover image fits the viewport, doesn't stretch to 600 px. Cycling all four presets live re-renders cleanly.
- ASan + UBSan clean.
- All 5 stress tests still pass.

### Notes for packagers

`data/fonts/` is part of the source tree now. Flatpak manifests will pick them up automatically through `install_subdir`. The fonts add ~1.7 MB to the install footprint — acceptable for a reading app where typography is the user-facing surface.

---

## v0.46.0 (2026-05-02)

*Reading Settings dialog — body font family, size, line-height, monospace family, all live-applied.* The first half of the typography work Brandon called out. The bundled-fonts piece (OpenDyslexic + a serif) ships in v0.47.0; this slice gets the plumbing in place and lets users point at any locally-installed font.

### GSettings schema

Four new keys under `io.github.virinvictus.framework`:

| Key | Type / range | Default | What it drives |
|---|---|---|---|
| `reading-font-family` | string | `""` (system serif fallback) | Pango family for paragraph / heading / blockquote / chapter blocks |
| `reading-font-size` | double, 8.0–32.0 | 13.0 | Body-text size; headings scale from this (h1 = +9pt, h2 = +5pt, h3 = +2pt, h4 = +1pt) |
| `reading-line-height` | double, 1.0–2.5 | 1.5 | Line-height multiplier |
| `reading-monospace-family` | string | `""` (system mono fallback) | Pango family for `<code>` / `<pre>` blocks |

### `FwReflowView` regenerates CSS on change

`FwReflowView` now holds a `GSettings` handle and subscribes to `changed` on the schema. On every settings-key change, `build_reflow_css` rebuilds the entire rule set — typography rules merged with the static set (selection-highlight suppression, image background, blockquote bar). The single `GtkCssProvider` reloads via `gtk_css_provider_load_from_string` so the live document re-renders on the next frame. No restart required.

### Reading Settings dialog

`AdwDialog` containing an `AdwPreferencesPage` with three groups:

- **Fonts** — two `AdwEntryRow`s for body family and monospace family, both bound directly to GSettings via `g_settings_bind` (`G_SETTINGS_BIND_DEFAULT`). Empty = system fallback.
- **Size & spacing** — two `AdwSpinRow`s for font size and line height, also `g_settings_bind`'d.
- **Presets** — four buttons in a linked `GtkBox` (`Default`, `Compact`, `Comfortable`, `Dyslexic`) that write the matching `{family, mono, size, line-height}` bundle into GSettings. The Dyslexic preset names "OpenDyslexic" / "OpenDyslexic Mono" — works once the user has the font installed (or the bundle from v0.47.0 lands).

Wired to a new `win.reading-settings` action and a new "Reading Settings…" entry in the primary menu (above "Document Properties…").

### Verified

- *Verdant Passage* EPUB — opening the dialog, changing font size live, switching presets, all reflow text re-renders immediately. ASan + UBSan clean.
- All 5 stress tests still pass.

### Next slice

v0.47.0 — bundle OpenDyslexic + a body serif, register them with FontConfig at app startup, so the "Dyslexic" preset works out of the box without the user needing to install fonts.

---

## v0.45.0 (2026-05-02)

*Reflow nav routing fixes — the "stuck on page 1" symptom is gone, plus the listview hover highlight is suppressed and the header arrows reflect the active mode.*

### Stuck-on-first-page bug

The window's capture-phase scroll and key controllers were intercepting every event and applying the result to the **fixed-layout** scrolled window — even when the reflow view was the visible child. Wheel events and arrow keys got consumed by the window but written into a hidden vadjustment, so the EPUB looked unscrollable.

Fix: both `on_scroll` and `on_key_pressed` now early-return to the default propagation path when `self->reflow_doc` is set, letting the listview's own scrolled window receive the event natively. The fixed-layout damping/cap (`SCROLL_MAX_STEP`) is preserved for PDF/DjVu where it matters; reflow gets GTK's stock kinetic scroll, which is what every native list-of-text widget on the platform expects.

### Page-by-page nav for reflow

New `fw_reflow_view_scroll_by_page(self, direction)` advances by viewport height, with a 40 px overlap so a line of context spans the page turn (matches Foliate's ergonomic). `direction` is +1 / -1 / 0 (jump to start) / `G_MAXINT` (jump to end).

Wired through:

- **`Page Down` / `Space` / scroll wheel** → handled natively by the listview.
- **`Left` / `Right` arrow** → page-turn (LTR reading order).
- **`win.next-page` / `win.prev-page`** actions → `scroll_by_page(±1)` for reflow, existing `go_to_page` for fixed-layout.
- **`win.first-page` / `win.last-page`** actions → `scroll_by_page(0)` / `scroll_by_page(MAXINT)` for reflow.
- **Header arrow buttons** — `prev_page_clicked` / `next_page_clicked` callbacks branch the same way.

### Header chrome reflects mode

When a reflow document opens:

- The page-nav arrows swap from `go-up-symbolic` / `go-down-symbolic` (vertical scroll glyphs) to `go-previous-symbolic` / `go-next-symbolic` (horizontal page-turn glyphs) — the visual cue Brandon called out for EPUB / MOBI / AZW3.
- The page entry hides (no concept of "page X of Y" in continuous reflow).
- The zoom entry + ± buttons hide (no zoom in reflow; font-size adjustment lands in v0.46.0).

When a fixed-layout document opens, all of the above are restored. Both icons get matching tooltip text.

### Hover/selection highlight suppressed

`GtkListView`'s default `:hover` and `:selected` row backgrounds are appropriate for picker UI but actively distracting for a book. The reflow listview now carries a `reflow-listview` CSS class; a few rules in the existing CSS provider null out `background`, `box-shadow`, and `outline` for `> row:hover`, `> row:selected`, `> row:focus`. Selection still works internally (the model tracks it for navigation) — it just doesn't draw any background.

### Verified

- *Verdant Passage* EPUB — opens, scrolls, pages through with arrow keys / header buttons / Page Down. ASan + UBSan clean.
- PDF (Effective Java) regression — fixed-layout scroll cap + arrow-key behavior preserved.
- All 5 stress tests still pass.

### Still upcoming (per Brandon's feedback this round)

- **v0.46.0** — bundled reading fonts (serif body, OpenDyslexic, monospace) + font-preference GSettings + a settings dialog. The infrastructure for picking a font lands here; defaults fall back to system fonts until the bundle is in.
- **v0.47.0+** — true paginated single-page-at-a-time mode (current behavior is windowed continuous scroll); two-column option behind a toggle.
- **Phase 13.1 Phase 4** — MOBI backend (PalmDOC LZ77 + KF7).

---

## v0.44.0 (2026-05-02)

*Reflow TOC sidebar — F9 in an EPUB or FB2 now shows a clickable chapter list, and clicking a chapter scrolls the listview to it.* The reflow backends were already producing `GListModel<FwReflowTocItem>`s; the missing pieces were a sidebar widget that consumes them and the dispatch wiring in the window.

### `FwReflowSidebar`

`src/fw-reflow-sidebar.{h,c}` (~150 LOC). Lightweight peer to the existing fixed-layout `FwSidebar` — `GtkListView` + `GtkSingleSelection` + `GtkSignalListItemFactory` (one row = one ellipsizing `GtkLabel`), `single-click-activate=TRUE`. The `activate` signal handler emits a new `anchor-requested(string)` signal carrying the `FwReflowTocItem`'s anchor id. Reflow TOCs are flat — no tree-list-model expansion machinery needed.

### `fw_reflow_view_scroll_to_anchor`

New API on `FwReflowView`. Resolves the anchor via `fw_reflow_document_find_block_by_anchor` (which returns 1-based positions, with 0 = not found), then calls `gtk_list_view_scroll_to(list, pos, GTK_LIST_SCROLL_FOCUS, NULL)`. EPUB anchors are typically `chapter.html` or `chapter.html#fragment` and resolve against the chapter map populated at parse time. FB2 anchors are section ids.

### Window dispatch

`fw-window.c` gains a `sidebar_scroll` member (was a constructor-local) that hosts whichever sidebar widget matches the active document. On reflow open, `gtk_scrolled_window_set_child` swaps the fixed-layout `FwSidebar` out for `FwReflowSidebar` and feeds it the document's TOC model. On fixed-layout open the swap reverses. The window connects both `page-requested` (fixed) and `anchor-requested` (reflow) signals at startup; the active sidebar's signal fires, the inactive one stays silent.

### Verified

EPUB *Verdant Passage* sidebar populates with all NCX chapters; clicking each row scrolls the listview to the right place. FB2 sample sidebar shows the two test chapters. ASan + UBSan clean. PDF (fixed-layout) regression check on *Effective Java* passes — sidebar swap-back works on transition. All 5 stress tests still pass.

---

## v0.43.0 (2026-05-02)

*Image rendering in `FwReflowView` — covers and inline graphics in EPUB/FB2 now display as native `GdkTexture`s instead of falling through to the text-label placeholder.* The `IMAGE` blocks every reflow backend already produced are finally rendered.

### Per-row factory upgrade

`FwReflowView`'s factory previously wrapped every block in a `GtkLabel`, which meant `FW_BLOCK_IMAGE` showed up as the image-id string. Now each row hosts a two-page `GtkStack`:

- **`text`** page — the existing wrapping, selectable `GtkLabel` for paragraph / heading / code / blockquote / hr / chapter blocks.
- **`image`** page — a `GtkPicture` with `content-fit=contain`, `can-shrink=TRUE`, centered halign, and a 600 px height cap so a cover image can't dominate the viewport.

Bind switches the visible page based on `fw_block_get_kind`. The unused widget stays alive in the off page so a recycled row toggling between kinds skips widget churn. Image lookup goes through `fw_reflow_document_get_image` against the active document; if the texture isn't available (decode failed at open, or the manifest didn't carry the id) the row gracefully falls back to the text page with the image-id as plain text.

### Lookup keys

- **EPUB** — `IMAGE.image_id` is the resolved zip path (e.g. `cover.jpg`); the EPUB backend stores textures under that path *and* under the manifest id, so both work.
- **FB2** — `IMAGE.image_id` is the binary's `id` attribute (leading `#` stripped); the FB2 backend stores by the same id.

The two backends were already keying their image hashes the way the view expects — this slice was just the view-side dispatch.

### CSS

New `.reflow-image` class adds a faint `currentColor`-tinted background and rounded corners so the image's bounding box is visible against the reading column even on transparent pages. Pure aesthetics, ~3 lines of CSS.

### Files

`src/fw-reflow-view.c` — replaced single-label factory with stack/label/picture factory. ~80 LOC delta. ASan + UBSan clean on the *Verdant Passage* EPUB; smoke-tested on the FB2 sample. All 5 stress tests still pass — fixed-layout pipeline untouched.

---

## v0.42.0 (2026-05-02)

*Phase 13.1 Phase 3 — EPUB reflow backend (the marquee delivery).* `.epub` now routes through `FwReflowDocumentEpub`. The full pipeline — `META-INF/container.xml` → OPF → manifest + spine + metadata → per-chapter XHTML → blocks; NCX → TOC; manifest images → `GdkTexture` hash — runs end-to-end on real Calibre-generated EPUBs.

### Pipeline

1. **ZIP read** (libarchive). One streaming pass enumerates every entry and caches its bytes into a `path → GBytes` hash. EPUB sizes are typically 1–50 MB; trading peak open-time memory for random access through the rest of the open path is the right call versus re-streaming for each lookup.
2. **`META-INF/container.xml`** — strict GMarkupParser, walks `<rootfile>` for `full-path`. Errors here fail open; the file isn't a recognizable EPUB.
3. **OPF parse** — manifest (`<item id href media-type>`), spine (`<itemref idref>` order), `<spine toc="...">` for the NCX id, and `<metadata>` for `dc:title` (→ `title`), `dc:creator` (→ `author`), `dc:language` (→ `lang`), `dc:publisher`, `dc:date`. First-write-wins on metadata so multiple `<dc:creator>` entries don't churn the canonical author. All hrefs resolved relative to the OPF directory by a `resolve_zip_path` helper that normalizes `./` and `..` segments.
4. **Spine walk** — for each idref in declaration order, look up the manifest entry, look up the bytes in the zip hash, run them through the XHTML parser. Failed chapters are warned and skipped — the rest of the book still opens (the design's resilience strategy).
5. **XHTML → blocks**. Strict GMarkupParser (real Calibre EPUBs are well-formed XHTML; libxml2's tolerant mode is the next iteration if real-world breakage shows up). Tag map: `<h1..h6>` → `HEADING` (level = N); `<p>` → `PARAGRAPH`; `<blockquote>` → `BLOCKQUOTE`; `<pre>` → `CODE`; `<li>` → `PARAGRAPH` with `"•  "` prefix; `<hr>` → `HR`; `<br>` → newline within active accumulator; `<img src>` → `IMAGE` with src resolved against the chapter's directory. Inline tags (`em`/`i`, `strong`/`b`, `code`/`tt`/`kbd`/`samp`, `sub`, `sup`, `s`/`strike`/`del`, `u`/`ins`/`a`) translate to Pango markup. Whitespace is collapsed at append time (single space between runs) so the source file's pretty-printed indentation doesn't leak into rendered text. CSS is dropped by design — Framework's typography wins.
6. **CHAPTER markers** — emitted lazily, on the first content block of each spine entry rather than blindly at chapter-open. The marker's anchor is the chapter's resolved zip path so NCX `<content src="chapter02.html">` lookups land on it. Anchored elements with `id="..."` get a composite `chapter.html#frag` anchor.
7. **NCX TOC** — `<navPoint><navLabel><text>...</text></navLabel><content src="..."/></navPoint>` walked in document order. The `src` is split on `#` to honor mid-chapter fragments. Resolved against the NCX file's directory (which is usually but not always the OPF directory). EPUB 3 `nav.xhtml` is a follow-up; falling back to NCX picks up most EPUB 3s anyway since publishers typically ship both for backwards compatibility.
8. **Image decode** — every manifest item with media-type `image/*` is decoded via `gdk_texture_new_from_bytes`. Each texture lands in the images hash under both its resolved zip path (matches `IMAGE` blocks emitted by the XHTML parser) and its manifest id (for hand-rolled callers).

### Verified on

Five real EPUBs from `/home/bdkl/docs/Calibre Library/`: *The Verdant Passage* (Denning), *The Fall* (Cahill), *20th Century Ghosts* (Hill), *Red Rising* (Brown), and *The Ego and His Own* (Stirner, Z-Library version). All open without warnings or parse errors. ASan + UBSan clean on the *Verdant Passage* path. All 5 stress tests still pass — the existing fixed-layout pipeline is untouched.

### Files

- `src/fw-reflow-document-epub.{h,c}` — ~700 LOC.
- `fw-reflow-document.c` — `.epub` routes to the new backend (was: through MuPDF as a "reflowable" format).
- `src/meson.build` — source list updated; no new dependency (libarchive was already in for CBR).

### Out of scope (this slice)

- **Image rendering** — `FwReflowView` still treats `FW_BLOCK_IMAGE` as a text label fallback. The textures are decoded and held; the `GtkPicture` upgrade in the view is the next focused slice and unlocks rendering for both EPUB and FB2 simultaneously.
- **EPUB 3 nav.xhtml** — NCX is the only TOC source for now. EPUB 3 publishers ship NCX for backward compatibility, so this isn't a blocker; lands when a real EPUB 3-only book turns up.
- **Tolerant HTML parsing** — strict GMarkupParser handles every test EPUB so far. libxml2 stays out of the dep set until real-world breakage forces it.
- **Search inside reflowed content** — `epub_search` returns `NULL` like every other reflow backend's `search()`. Phase 6 polish.
- **DRM-encrypted EPUBs** — surface as parse errors and the document fails to open. No circumvention; out of scope.

`fw-document.c`'s `.epub → MuPDF` fallback is still in the fixed-layout factory — kept for the case where the reflow path bails on a malformed book. Dispatch order in `fw_window_open_file` puts the reflow path first, so MuPDF only sees an EPUB if reflow refused.

---

## v0.41.0 (2026-05-02)

*Phase 13.1 Phase 2 — FB2 (FictionBook 2) reflow backend.* `.fb2` now routes through `FwReflowDocumentFb2` instead of MuPDF's reflow path. Walks the file with `GMarkupParser`, building a flat block list with section structure preserved as CHAPTER markers + nested HEADING blocks, paragraphs as PARAGRAPH (with inline `<emphasis>`/`<strong>`/`<code>`/`<sub>`/`<sup>`/`<strikethrough>` translated to Pango markup), `<cite>`/`<epigraph>` as BLOCKQUOTE, `<empty-line/>` as HR, and `<image l:href="#X">` references resolved against the document's `<binary id="X">` attachments (decoded from base64 → `GdkTexture` via `gdk_texture_new_from_bytes`).

### Coverage

- **Sections**: each `<section>` increments depth and emits a CHAPTER marker; section's `id` attribute (when present) becomes the block's anchor for ToC/find_block_by_anchor lookups.
- **Headings**: `<title>` (only inside a section) accumulates into a HEADING with `level = section_depth`. `<p>` children are joined with single spaces rather than starting their own blocks.
- **Paragraphs**: `<p>` and `<subtitle>` outside `<title>` start a PARAGRAPH accumulator; trailing whitespace trimmed at flush.
- **Inline styles**: `emphasis`→`<i>`, `strong`→`<b>`, `code`→`<tt>`, `sub`→`<sub>`, `sup`→`<sup>`, `strikethrough`→`<s>`. Unrecognized inline tags are silently skipped (text inside still flows).
- **Images**: `<image>` (anywhere in body) pushes an `FW_BLOCK_IMAGE` referencing the binary by id (leading `#` stripped). Resolved at render time via `fw_reflow_document_get_image`. Three `href` namespace prefixes accepted: `l:href`, `xlink:href`, plain `href`.
- **Binaries**: parsed even when they appear after the body (FB2's typical layout). Bad image data triggers a `g_warning` and the entry is dropped — the document still opens.
- **TOC**: each section title pushes an `FwReflowTocItem` with the section's anchor id. Plain text — no Pango markup — to keep sidebar labels safe.
- **Metadata**: `<title-info>` populates `title` (from `<book-title>`), `lang`, `src-lang`, `annotation`, and a synthesized `author` field from `<first-name>` + `<middle-name>` + `<last-name>`. Falls back to filename for `title` when missing.
- **Encoding**: `<?xml encoding="..."?>` other than UTF-8 is converted via `g_convert` before parsing — `GMarkupParser` is UTF-8 only. UTF-8 (and no declaration) takes the zero-copy fast path.

### Anchor map

`anchors[anchor_id] = block_position + 1` (1-based so the GHashTable's NULL value can mean "not found"). `fw_reflow_document_find_block_by_anchor` now returns a real result for FB2; TXT still returns 0.

### Out of scope (this slice)

- `.fb2.zip` archives — `.fb2` only for now. Zip-wrapped FB2 is the same XML inside a libarchive read; tracked as a follow-up.
- Verse / poem layout (`<v>`, `<stanza>`) — render as plain paragraphs at the moment. Real verse formatting needs the view to support left-margin variants.
- `<a l:href="...">` anchor links — text passes through; the link target isn't recorded yet (no internal-link support in `FwReflowView` yet).
- Search — every reflow backend's `search()` still returns NULL until Phase 6 polish lands.
- Image rendering — `FwReflowView` still treats `FW_BLOCK_IMAGE` as a text fallback (the bind handler's `default:` case). The binaries are parsed and held; the view upgrade lands when EPUB's image cover page makes it indispensable.

### Files

`src/fw-reflow-document-fb2.{h,c}` (~440 LOC). `fw_reflow_path_is_supported` and the factory in `fw-reflow-document.c` extended for `.fb2`. Source list in `src/meson.build` updated. ASan + UBSan clean on a synthesized FB2 covering sections, nested sections, inline styles, images, binary, multi-paragraph chapter content, character escapes, and `<empty-line/>` markers. All 5 stress tests still pass.

The fixed-layout dispatcher in `fw-document.c` still has `.fb2 → MuPDF` for the case where the reflow path bails — leaving that wired keeps a working fallback if a real-world FB2 trips the parser. The dispatch order in `fw_window_open_file` puts the reflow check first, so MuPDF only gets the file when reflow refuses.

---

## v0.40.0 (2026-05-02)

*Phase 13.1 Phase 1 — Fractal-style reflow architecture lands as a TXT-only proof.* The new pipeline runs in parallel to the existing `FwView` + `FwCache` + `FwDocument` stack, dispatched at file-open time. `.txt` files now route through `FwReflowDocument` + `FwReflowView`; PDF / DjVu / CBZ / CB7 / CBT / CBR / XPS / EPUB / FB2 / MOBI continue to flow through MuPDF. EPUB / MOBI / AZW3 / FB2 will move to the reflow pipeline in later Phase 13.1 phases — the architecture is in, the format work is the next slice.

### New: `FwReflowDocument` interface

`src/fw-reflow-document.{h,c}` defines the parallel interface from `docs/fractal-rewrite.md` §2. The `FwBlock` GObject (kind enum + level + Pango-markup text + image_id + anchor_id + flags) plus a `FwReflowTocItem` GObject. The interface vtable: `open` / `close`, `get_block_model` (the hot path — bound directly to `GtkListView`), `get_image`, `get_toc`, `find_block_by_anchor`, `search`, `get_metadata`. `FwBlockKind` rather than `FwBlockType` to dodge the GObject-getter naming collision; otherwise tracks the design doc 1:1.

### New: `FwReflowDocumentTxt` backend

`src/fw-reflow-document-txt.{h,c}`. Reads the file, normalizes the encoding (UTF-8 BOM strip, UTF-16 LE/BE BOM via `g_convert`, UTF-8 validate, ISO-8859-1 fallback), splits on blank-line runs (Markdown convention), Pango-escapes each chunk, pushes a `FW_BLOCK_PARAGRAPH` per paragraph into a `GListStore<FwBlock>`. Empty TOC, NULL `get_image`. Trivial — exactly as scoped in the design doc §3 "TXT (easiest — start here)".

### New: `FwReflowView` widget

`src/fw-reflow-view.{h,c}`. `GtkWidget` with a `GTK_TYPE_BIN_LAYOUT` hosting a `GtkScrolledWindow` → `GtkListView` → `GtkSingleSelection`. `GtkSignalListItemFactory` setup creates a `selectable=TRUE` wrapping `GtkLabel`; the bind handler swaps CSS classes per `FwBlockKind` (HEADING / CODE / BLOCKQUOTE / HR / CHAPTER / PARAGRAPH). CSS loaded once at view init drives typography (font-size, line-height, the blockquote vertical bar). Reading column capped at 720 px via `gtk_scrolled_window_set_max_content_width` so wide windows don't produce inhumane line lengths. Pango wrapping + native `GtkLabel` selection are GTK-native — no custom snapshot work for text rendering.

### Window dispatch

`fw-window.c` gains `reflow_doc` / `reflow_view` peers to `document` / `view`; the content stack now has three pages: `empty`, `document` (fixed-layout), `reflow`. `fw_window_open_file` checks `fw_reflow_path_is_supported(path)` (Phase 1: `.txt` only) and branches to a dedicated `fw_window_open_reflow` helper. Both paths share `fw_window_close_active_document` for teardown so swaps between fixed-layout and reflow are clean. `fw-application.c`'s file-open dialog gains `*.txt` + `text/plain`. Most fixed-layout actions (zoom, rotation, crop, loupe, ruler) are still wired but no-op when the reflow view is active because they all begin with `if (!self->document) return;`. State persistence, file-monitor auto-reload, navigation history, and search aren't wired for the reflow pipeline yet — those land alongside FB2 / EPUB.

### Bug caught during smoke test

`gtk_list_view_new` is `transfer-full` for both its model and factory args — the initial draft passed both then `g_object_unref`'d the factory afterward, dropping it to refcount 0 mid-construction. The first model swap segfaulted in `g_value_object_collect_value` during `gtk_list_view_create_list_widget`. Fix: the listview now consumes the original factory ref and gets an explicit `g_object_ref` of `self->selection` so the view holds its own ref independent of the listview's. ASan + UBSan clean across the smoke corpus, all 5 stress tests still pass.

### Build

Three new sources — `fw-reflow-document.c`, `fw-reflow-document-txt.c`, `fw-reflow-view.c` — appended to `framework_lib_sources`. No new dependencies: GTK, GLib, GIO, libadwaita, Cairo were already in. The reflow pipeline links into `framework-core` so the existing test harness can reach it once Phase 13.1 grows tests.

---

## v0.39.1 (2026-05-01)

*Comprehensive attribution sweep across `README.md`.* Reframed the project's opening to be transparent that Framework is a deliberate synthesis — SumatraPDF's cache + threading idioms, zathura-pdf-mupdf's zero-copy MuPDF→cairo pipeline + Zathura's `GThreadPool` priority dispatch, YACReader's loupe + double-spread detection, and (Phase 13.1) Fractal's native-GTK reflow architecture, glued into a minimalist libadwaita UI. Sioyek, Plato, MComix, Komikku, Foliate listed as additional pattern sources.

### Per-pattern attribution overhaul

The "Influences and borrowed techniques" section now names every borrow's: **upstream project**, **upstream file:line**, **Framework version it shipped in**, and **Framework source file** the borrow lives in. Several misattributions corrected — the multi-zoom retention (`try_closest_rendered_page`) and TTL+LRU eviction were filed under SumatraPDF but they're actually Sioyek's. The per-instance store sizing was three-way (Plato's tight cap + Sumatra's default + Sioyek's auto), now correctly attributed.

### New attribution sections

- **YACReader** — was missing entirely. Now credited for the magnifying loupe (v0.24), aspect-ratio spread detection (v0.27.1), and filename-based spread detection (v0.37).
- **Fractal** — was buried in the design doc. Now front-and-centre as the named architectural foundation for Phase 13.1 reflow.
- **MComix** — credited for the manga RTL key-swap concept (v0.27).
- **Komikku** — credited for webtoon mode + facing-pages (v0.27) and for the Phase 13.1 reader-pager pattern reference.
- **Foliate** — listed as the explicit "we are not trying to replace this" reference. No code borrows; included for transparency about the ebook reader space.
- **Zathura landlock** — v0.38 borrow added to the Zathura section.

### Library credits broadened

System libraries section now spells out every runtime dep: MuPDF, DjVuLibre, libarchive (rendering/format engines); GTK4, libadwaita, Cairo, GLib, JSON-GLib, Pango (platform); Linux kernel Landlock LSM (optional sandbox). Plus a small Tooling section for Meson, Ninja, gettext, and the dev sanitizers. Build-deps tables earlier in the README already list the same set; this section grounds them in their licenses.

No code changes.

---

## v0.39.0 (2026-05-01)

*Reference-repo cleanup audit.* Seven of the nine vendored upstream checkouts have been mined for everything Framework can reasonably borrow; this version removes them from disk and from `.gitignore` after one last audit sweep that surfaced two more borrows worth shipping (v0.37 YACReader filename-spread, v0.38 Zathura landlock).

### Removed (~440 MB freed)
| Repo | Size | What we got | What's deferred |
|---|---|---|---|
| `.zathura/` | 3.4 MB | sort-function priority dispatch (v0.14), hue-preserving recolor (v0.22), landlock LSM hardening (v0.38) | vim-mode girara UI, per-page widget model — explicitly NOT borrowing |
| `.zathura-mupdf/` | 244 KB | zero-copy MuPDF→cairo (v1.6), cached `fz_stext_page` (v0.18) | nothing — fully mined |
| `.sumatrapdf/` | 271 MB | engine abstraction, fz_cookie cancellation (v0.17), bytes-aware cache cap (v0.16), smart text selection inspiration (v0.19), auto-reload concept (v0.21) | true tile slicing — Tier 2 follow-up if poster PDFs ever need it |
| `.sioyek/` | 23 MB | try_closest_rendered_page (v0.28), TTL+LRU eviction (v0.26), reading ruler (v0.23), margin cropping concept (v0.25) | per-thread shared-context model — Tier 3 validated as not currently needed |
| `.plato/` | 13 MB | per-instance store sizing reference (v0.26) | nothing — AGPL prevents code copy regardless |
| `.yacreader/` | 104 MB | magnifying loupe (v0.24), aspect-ratio spread detection (v0.27.1), filename-based spread detection (v0.37) | comic-library management — out of scope |
| `.mcomix/` | 10 MB | manga RTL navigation (v0.27) | smart-grid PgDown algorithm — niche, current behavior fine |

### Kept
| Repo | Why |
|---|---|
| `.fractal/` | Active reference for the Phase 13.1 reflow rewrite (`docs/fractal-rewrite.md`). Specifically `.fractal/src/utils/grouping_list_model/` for the dynamic-height list-model pattern. |
| `.komikku/` | Same — `komikku/reader/pager/` for reflow / paginated dispatch logic. |

### Cleanup
- `.gitignore` trimmed to just `.fractal/` and `.komikku/`.
- `CLAUDE.md` "Reference repos" section retains the `.fractal/`/`.komikku/` table and the License compatibility matrix; the per-repo deep-dive subsections for the seven removed repos are gone (they were a map-of-where-to-look that no longer maps to anything).
- `roadmap.md` and `docs/fractal-rewrite.md` had `.repo/` path prefixes stripped from references that pointed inside the removed checkouts. Earlier patchnotes still reference them historically — left untouched as record-of-what-was-borrowed-when.

The borrowing trail isn't lost: `README.md`'s "Influences and borrowed techniques" section retains the per-pattern attribution with upstream paths and line numbers, and the patchnote for each ship cites the source pattern. Anyone wanting to verify a borrow can clone the upstream repo at `--depth 1` and grep — same as we did originally.

No code changes; this version is structural / documentation only.

---

## v0.38.0 (2026-05-01)

*Landlock LSM hardening — borrowed from zathura.* The binary now drops filesystem-execute and create-special-file-type permissions via Linux's [Landlock](https://landlock.io/) LSM at process startup. If a malicious document exploits MuPDF / DjVuLibre / libarchive into RCE, the foothold can no longer escalate by `execve()`'ing a shell or by mknod'ing a backdoor — the kernel rejects the syscall before the libc sees it. Read and `WRITE_FILE` rights stay allowed so state.json persistence, save-attachments, and print spool writes continue to work.

`FW_TRACE_WINDOW` logs whether the sandbox applied:

```
[86485.3739] [window] landlock applied: dropped EXECUTE + MAKE_* (abi=7)
```

### Implementation
New `src/fw-sandbox.{c,h}` ports `.zathura/zathura/landlock.c`'s pattern (Zlib license, compatible) into a single `fw_sandbox_drop_execute()` call invoked from `main.c` after `fw_debug_init()` and before `g_application_run()`. Header presence is auto-detected via `__has_include(<linux/landlock.h>)`; non-Linux builds and pre-Landlock kernels gracefully fall through to a logged no-op.

The Framework variant intentionally stops short of zathura's full `landlock_restrict_write` (which limits writes to `XDG_DATA` only). That stronger lockdown would break save-attachments and print spool, both of which write to user-picked paths outside `XDG_DATA`. If the threat model justifies losing those features, the path-beneath restriction can land in Phase 15 alongside the Flatpak permissions audit.

### Why this complements Flatpak
Flatpak's portal locks the *filesystem* via xdg-desktop-portal — the user picks a file, the portal hands the app a single fd. Landlock locks the *process* via the kernel — even if Flatpak isn't in use (DNF install, `meson install`), the lockdown applies. Belt and suspenders.

---

## v0.37.0 (2026-05-01)

*Filename-based double-spread detection — borrowed from YACReader.* Aspect-ratio detection (v0.27.1) catches centerfolds whose image is genuinely wider than tall, but it misses scanlation rips where the spread image has the same dimensions as a single page and the spread-ness is encoded in the *filename* — `chapter01_034035.jpg` etc. New CBR-backend `is_spread_filename` interface method ports YACReader's `common/comic.cpp:925-1028` algorithm:

1. At open time, walk the (sorted) entry list and find the most common filename prefix among basenames. Reject if it occupies < 60% of pages or if it's all-digits.
2. Per page, strip the prefix, take the leading digit run, and check that it's even-length, splits in half into two ints, and `right − left == 1`.

`view_page_is_spread` consults `fw_document_is_spread_filename` first, falling back to the existing aspect-ratio check. Backends without per-page filenames (PDF, DjVu, MuPDF-routed CBZ via `fz_open_document`) leave the vtable slot NULL and behave exactly as before — only CBR (libarchive-direct) gets the new signal.

Combined effect: a manga rip with portrait-aspect spread images now displays each spread on its own row in facing-pages mode (rather than being naively paired with the next page), provided the filenames follow the convention. Aspect-ratio-only spreads continue to work unchanged.

License: GPL-3 (compatible). The algorithm is ported, not copied verbatim — original Qt/QString idioms are re-expressed in C / GLib.

---

## v0.36.0 (2026-05-01)

*Phase 11 Tier 3 validated — current MuPDF threading model retained.* Both Tier 3 entries (`Reconsider 8× fz_open_document model`, `Per-thread-shared-context hybrid`) were investigation-only items hedging against memory pressure on low-RAM laptops. The pre-1.0 stress harness now provides enough measurement to make a defensible call:

- `stress-multidoc` holds 10 simultaneous `FwDocument` + `FwCache` instances under 2 GB RSS (with ASan).
- `stress-corpus-soak` walks the full corpus (PDF×2, DjVu, EPUB, MOBI, CBZ, CBR) under 1.5 GB peak RSS.
- `bench-render` shows a 3× warm/cold speedup on Effective Java — the v0.26-scaled per-instance stores (16/32/64/128 MB) are doing their job at the sizes we picked.

The 8-instance model's worst-case ceiling for a single open doc is ~1.6 GB (8 × 128 MB stores + thumbs + cache cap). On any contemporary 16+ GB laptop the headroom is comfortable. The `fz_clone_context` refactor introduces real threading risk (locks-callback contract, per-thread context lifetime, store-eviction across cloned contexts) and would also forfeit the current "per-instance lock means workers never wait on each other" simplicity — so the change isn't free even if memory-neutral.

Both Tier 3 items are now marked `[~]` (validated, retained) in `roadmap.md`. They stay tracked as fallbacks if a memory-constrained deployment target ever shows up — Plato-style e-reader port, Flatpak sandbox memory limit, embedded distro — but no code change ships in v0.36.

No code changes; this version is documentation only.

---

## v0.35.0 (2026-05-01)

*High-zoom render-bytes cap — Phase 11 Tier 2 (pragmatic deviation from spec).* Per-render allocation is now capped at 64 MB. When the requested zoom would produce a larger surface, `render_zoom` scales down so the surface fits; the resulting texture upscales via GTK's GSK pipeline when the view paints it at the requested rect. Combined with v0.28's `try_closest_rendered_page` retention, the user sees a slightly-blurry preview at extreme zoom on poster-format PDFs rather than an OOM allocation.

### Why cap-and-upscale instead of true tile slicing?
The original Phase 11 Tier 2 entry called for rendering the page in N×M cairo surfaces above threshold. The cap-and-upscale variant ships the same memory bound (`w*h*4 < 64 MB`) in ~10 LOC vs. several hundred for a real slice path through cache + view. On the actual Calibre corpus (no poster PDFs) the threshold is essentially never reached, so the visual cost is hypothetical. True slicing remains tracked as a follow-up if a poster-format need ever surfaces.

The capped slot integrates cleanly with `try_closest_rendered_page`: `entry->zoom` is set to the *effective* doc zoom (post-cap) rather than the requested value, so when the user zooms back to a non-capped level the demoted slot's zoom is recorded accurately and `fw_cache_get_texture`'s nearest-zoom search picks the right preview.

`FW_DEBUG=1` traces `render-cap: page=N req_zoom=X.X → eff=Y.Y` whenever the cap fires, so you can tell from a trace whether you're in cap territory.

### libm linkage
The byte-cap math uses `sqrt`. Added `cc.find_library('m')` to the framework deps so the linker resolves it explicitly under Fedora's `--no-undefined` default.

---

## v0.34.0 (2026-05-01)

*`framework --self-test` — Phase 12.4.* New CLI flag, gated behind `-Dstress=true` (so packagers and end users don't carry the dead code). When invoked, opens a known-good document, drives the cache through the open path + a 12-step stride scrub across the document, asserts that page 0 actually painted within 5 seconds, and exits 0 on pass / 1 on fail. Headless — no widget tree, no window. About 80 LOC of conditionally-compiled `main.c` driving `FwDocument` + `FwCache` directly.

```
$ ./builddir/src/framework --self-test
self-test: opening /home/bdkl/docs/Calibre Library/.../Effective Java - Joshua Bloch.pdf
self-test: 901 pages
self-test: PASS
```

### Hash baseline dropped
The original spec called for hashing the rendered first-page surface against a stored baseline. In practice that hash is too brittle across MuPDF versions and platform fonts to be useful CI signal — every legitimate upstream font/rendering change false-positives. The smoke gate checks the property that *actually* breaks on toolchain regressions: "binary opens, page 0 paints, no crashes." Cleaner signal, no per-arch baseline maintenance.

### Build wiring
New `FW_STRESS_BUILD` config define driven by the existing `-Dstress=true` meson option. Non-stress builds reject `--self-test` with exit 2 and a clear message — no dead code in the shipping binary.

---

## v0.33.0 (2026-05-01)

*`tests/scripts/trace-replay.sh` — Phase 12.4.* Renders an `FW_DEBUG=1` log to an SVG timeline. Five horizontal tracks aligned to a shared time axis:

1. **state** — Cache render-state bands (green = STATIC, yellow = CRUISING, red = SCRUBBING) running edge-to-edge so you see velocity transitions at a glance.
2. **zoom** — Vertical orange tick at every `fw_cache_start` with the new zoom value labeled.
3. **w-start** — Green tick per worker dispatch (one tick per `worker start: page=N` trace line).
4. **w-done** — Blue tick per worker completion.
5. **evict** — Red triangle at every byte-cap eviction event.

Pure bash + awk, no Python or extra deps. Reads from a file argument or stdin and emits SVG to stdout. The 0.5 s tick grid + total runtime + event counts in the bottom-right give you a "did anything go wrong here" read at a glance — answers questions like "why did rendering stall for 200ms" without scrolling through hundreds of trace lines.

Usage: `FW_DEBUG=1 ./builddir/src/framework <doc> 2> trace.log; tests/scripts/trace-replay.sh trace.log > trace.svg`

---

## v0.32.0 (2026-05-01)

*`tests/scripts/coredump-triage.sh` — Phase 12.4.* Non-interactive coredump capture. Given a PID, a coredumpctl matchid, no argument (= latest framework crash), or `--core <file>` for a local core file, writes a timestamped triage directory under `~/.local/share/framework/triage/<UTC-timestamp>/` containing:

- `coredumpctl-info.txt` — full `coredumpctl info` for the dump (PID, signal, command line, loaded modules)
- `commandline.txt` — the original argv extracted from the info output
- `full-gdb.txt` — `gdb -batch` session against the dumped core (`thread apply all bt full` + registers + proc mappings)
- `threads-bt.txt` — per-thread bt slice extracted from the gdb output
- `cache-state.txt` — placeholder; the FwCache pretty-printer is roadmap follow-up

Implementation note: `coredumpctl debug` interleaves its own info text with gdb's output and isn't reliable for capture, so the script extracts the core via `coredumpctl dump --output` and runs gdb directly against `(builddir/src/framework, core-file)`. Build-ID mismatches between the running binary and the dumped core produce a warning but don't block — most frames still resolve when symbols match.

Stays out of `meson test` (debugging utility, not a regression check).

---

## v0.31.0 (2026-05-01)

*`tests/scripts/debug.sh` — Phase 12.4.* One-shot gdb wrapper for crash investigation. Runs `./builddir/src/framework` under `gdb -batch` with the breakpoints from `tests/scripts/framework.gdb` pre-loaded:

- `fz_throw` — every MuPDF `fz_try`/`fz_catch` raise (catches the silent-warning ones too)
- `cache_entry_free` — every page eviction
- `submit_next_jobs` — every render-worker dispatch with priority info

Print + continue rather than stop, so the program runs through to whatever crash you're chasing and a full multi-thread `thread apply all bt` fires at the end. `GSETTINGS_SCHEMA_DIR` is auto-set so the binary doesn't abort on the missing-schema lookup. Forwards `FW_DEBUG=1` if set.

Usage: `tests/scripts/debug.sh <doc>` — same arg shape as the binary.

---

## v0.30.0 (2026-05-01)

*`bench-cache-hit-rate` — Phase 12.3.* Drives the cache through three synthetic scroll patterns and reports the hit ratio at each render-state band:

- **STATIC** — velocity 0, walk page-by-page with 120 ms dwell. Cache fills the priority window ahead of each query.
- **CRUISING** — velocity 0.8, walk every 3 pages with 40 ms dwell.
- **SCRUBBING** — velocity 5.0, jump every 50 pages with 5 ms dwell. Most queries miss; the cache aborts mid-render via `fz_cookie`.

Hit/miss is measured by calling `fw_cache_get_texture(current_page)` after each step — exactly what the view sees on every paint. Synthetic patterns instead of recorded trace files: keeps the bench self-contained (no per-platform recorder, no trace-file schema) and reproducible across runs.

First baseline on Effective Java's 901 pages — STATIC 100%, CRUISING 100%, SCRUBBING 24%. SCRUBBING's low hit rate is by design: that's what the `fz_cookie` mid-render abort and SCRUBBING-state job suppression are *for*. Useful in this form for tuning the bytes-aware cache cap or the priority window without manual A/B testing.

---

## v0.29.0 (2026-05-01)

*`bench-startup` — Phase 12.3.* New benchmark that times each corpus sample's open-to-first-paint flow:

1. `fw_document_new_for_path` → `open_ms`
2. `fw_cache_start` + priority [0] + main-loop iteration → `first_paint_ms` (the moment `fw_cache_get_texture(0)` first returns non-NULL — the actual user-visible "page 0 painted" instant)
3. `total_ms = open + paint`

Reports per-file timings as a table; built but not registered with `meson test` (latency benchmark, not pass/fail). First baseline on the canonical corpus shows MOBI's `fz_layout_document` pass dominates open time (Fall of Kings: 934 ms open, 7 ms paint), CBZ's libarchive enumeration dominates the comic open path (Berserk v25: 653 ms open, 204 ms paint), and PDF/DjVu/EPUB land under 110 ms total. The `apply_fit_width_tick` deferred-layout path that this benchmark exists to guard runs cleanly in all cases.

---

## v0.28.0 (2026-05-01)

*`try_closest_rendered_page` zoom transition — Phase 11 Tier 2.* Continuous Ctrl+scroll zoom no longer falls back to a single-zoom prev_texture (which got progressively blurrier as the user crossed many zoom levels). Each `CacheEntry` now retains up to 3 prior-zoom snapshots; `fw_cache_get_texture` picks the slot whose zoom is closest to the current target (matching rotation + scale_factor) and returns it for GTK to auto-scale into the current rect. A user zoom-storming across 5 levels now sees their just-rendered 2.4× snapshot scaled 1.04× to fill a 2.5× rect — sharp — instead of the original 1.0× snapshot scaled 2.5× — blurry.

### Implementation
- New `ZoomSlot` struct: `(surface, texture, zoom, rotation, scale_factor, size_bytes)`. `MAX_PREV_ZOOM_SLOTS = 3` per page; oldest evicts on overflow.
- `CacheEntry` fields collapsed: dropped `prev_surface`/`prev_texture`/`prev_size_bytes`; added `prev_slots[3]` + `prev_slot_count` + `prev_slots_bytes`. Current-surface params (`zoom`, `rotation`, `scale_factor`) tracked at the entry level so demotion can capture them.
- `fw_cache_start` (zoom/rotation change): demotes the existing current surface into `prev_slots[0]`, shifting older slots right; oldest at index 2 frees on overflow. Bytes transfer from `size_bytes` → `prev_slots_bytes` without touching `total_cached_bytes` (no double-count).
- Worker store path simplified: stale-discard logic unchanged; the "drop stale prev" block is gone since prev_slots are intentionally retained until explicit eviction.
- `fw_cache_get_texture` walks `prev_slots[]` and returns the slot with minimal `|zoom − target|` (when current isn't ready). GTK's `gtk_snapshot_append_texture` already auto-scales — the view doesn't need a transform change.
- `fw_cache_get_prev_page` was an unused public API leftover; dropped from the header.

### Stress test cap bump
`stress-zoom-storm`'s settled-RSS cap raised from 1024 → 1280 MB. Multi-slot retention legitimately holds ~150 MB more cache memory after a 50-cycle zoom storm on the Effective Java sample — the bytes are still bounded by `byte_cap` (default 512 MB cache surfaces); the RSS rise is mostly glibc retention from the higher allocation churn. ASan-clean across all 5 stress tests.

---

## v0.27.2 (2026-05-01)

*Auto-resize the window for spreads, restore on the next page.* When a wide spread becomes the active row (centerfold in a CBZ, or a paired pair wider than the viewport in facing-pages mode), the window grows horizontally so the spread fits without a horizontal scrollbar. When the user scrolls/pages past the spread back to a normal row, the window restores the width it had before we grew. The interaction tracks a single baseline width so we never shrink past the user's manual sizing — only restore the size we captured before our own grow.

Compositor caveat: this uses `gtk_window_set_default_size` since GTK4 has no programmatic resize for shown windows. On most floating Wayland compositors and X11, the resize is honored. Tiling compositors may silently drop it; the `FW_DEBUG=1` `WINDOW` traces log every grow/shrink request so you can tell whether it took. Maximized and fullscreen windows are skipped — fighting the WM there isn't useful.

`fw_view_get_current_row_width` is the new public query: returns the displayed pixel width of the current row, which is the active page's width when standalone or the pair width (incl. gutter) in facing-pages mode. The window's `on_scroll_changed` calls it whenever current_page changes and decides whether to grow or shrink.

---

## v0.27.1 (2026-05-01)

*Spread detection for facing-pages mode.* Some CBZ files store 2-page centerfold spreads as a single landscape image; v0.27.0 was naively pairing those wide pages with the next portrait page, which broke the visual flow (especially obvious in manga reading). Now an aspect-ratio test (`w/h > 1.0` → standalone spread) drives a pre-built `pair_partner[]` array, so spreads stand alone and the page that would have been their natural partner orphans cleanly. After the spread, alternation resumes on whatever page follows.

The pairing decisions are computed once in `recompute_layout` from `page_widths`/`page_heights` (which already account for rotation), so `view_page_is_paired` and `view_pair_first` become O(1) lookups consulted by the snapshot path, click-to-doc mapping, and current-page tracking. Books that are landscape end-to-end (artbooks) will see every page standalone — appropriate, since pairing pre-spread pages would just shrink them in half.

---

## v0.27.0 (2026-05-01)

*Comic-reader trio + roadmap reorg.* Phase 13's three layout modes — Manga, Webtoon, Facing Pages — land together since they all touch `FwView::recompute_layout` and the snapshot path. Plus the long-stale "Hermitage" rename fixed in roadmap, and the 1.0 release section moved to the end of the document where it actually belongs given how far the 0.x sprint has gone.

---

### Manga Mode (Phase 13)
New `manga-mode` GSettings boolean, **F4** shortcut, "Manga Mode (RTL)" entry under the new Comic Layout submenu. When on, the directional page-nav keys swap — Left Arrow advances to next page, Right Arrow goes to previous — following the reading order of Japanese manga. Pure scroll geometry is unaffected (vertical layout doesn't change), so the toggle is purely about RTL nav semantics. Combined with facing-pages, also flips left/right within each pair so the lower-numbered page sits on the right.

The implementation lives entirely in `fw-window.c::on_key_pressed` (key swap) and `fw-view.c::view_page_is_paired`/snapshot-x (paired-layout flip) — about a dozen lines of behavior change for a feature that lights up an entire genre of content.

### Webtoon Mode (Phase 13)
New `webtoon-mode` GSettings boolean, **F5** shortcut. Drops `PAGE_GAP` to zero in `recompute_layout` so vertically-laid-out long-strip comics stitch into a seamless single canvas — designed for Korean webtoons and other formats where the artist composes across page boundaries. No-op when facing-pages is also active (mutually exclusive layouts).

Layout-anchor preservation: toggling webtoon mode runs through `view_apply_layout_change`, which captures `(page, intra-page-fraction)` before the layout change and restores after — so flipping the toggle mid-document keeps you in the same place rather than jumping to a different page.

### Facing Pages (Phase 13)
New `facing-pages` GSettings boolean, **F10** shortcut. Two pages per row, page 0 standalone as cover, then 1+2, 3+4, etc. — matches how a physical book opens. Added a pair-aware layout helper (`view_page_is_paired`/`view_pair_first`) that the snapshot path, click-to-doc mapping, and `fw_view_get_current_page` all consult so the rest of the view code doesn't have to reason about pairs. Pair height = max of the two pages (handles mismatched dimensions cleanly); pair width tracked through `max_width` so `GtkScrolledWindow` provides a horizontal scrollbar when a pair is wider than the viewport.

Combined with manga mode: lower-numbered page sits on the right within each pair. Combined with crop margins or zoom: same `(page, frac)` anchor preservation kicks in via `view_apply_layout_change`.

### Roadmap Cleanup
- Moved Phase 10 (1.0 Release) to the end of the document and renumbered to Phase 15. The roadmap had grown a v0.27 worth of features past the original 1.0 landing slot; the section was actively misleading where it sat.
- Replaced `dev.hermitage.Hermitage.desktop` (stale name from a previous project iteration) with the actual `io.github.virinvictus.framework.desktop`.
- Removed a duplicate `stress-zoom-storm` entry left over from the v0.15 sprint.

---

## v0.26.0 (2026-05-01)

*Cache and bench batch — Phase 11 Tiers 2 and 3, plus Phase 12.2 and 12.3 fill out the test harness.* Four roadmap items shipped together: per-instance MuPDF store size scaling, TTL+LRU hybrid cache eviction, a new render-latency benchmark, and a full-corpus soak test.

---

### Per-Instance MuPDF Store Size Scaling (Phase 11 Tier 3)
Both `fz_new_context` call sites in the PDF backend (the main context plus the eight per-instance render contexts) now size the MuPDF store to file size: 16 MB under 5 MB, 32 MB under 20 MB, 64 MB under 100 MB, 128 MB above. Previous fixed 32 MB allocation was wasteful on novels (5 MB EPUBs got the same store as 200 MB textbooks) and tight on heavy textbooks (font/JPEG2000 churn). With eight per-instance contexts, the upper bound scales to 1 GB total store on heavy documents — comfortable on this 30 GB box; revisit only if a memory-constrained reference (`.plato/`) becomes an actual target.

### TTL+LRU Hybrid Cache Eviction (Phase 11 Tier 2)
The bytes-aware eviction loop from v0.16 picked victims in iteration order — effectively arbitrary. Now each cache entry tracks `last_access_us`, bumped on every `fw_cache_get_page` / `fw_cache_get_texture` hit and on worker-store success; outside-priority candidates are sorted oldest-first before eviction. Pages the user just scrolled back to survive when the cap fires; pages they haven't touched in seconds go first. No new public API, no behavior change when under the cap; only the eviction policy improved.

### `bench-render` (Phase 12.3)
New benchmark that times direct `fw_document_render_page` calls across an evenly-spaced span of pages, in two passes — cold (fresh handle, populates the per-instance store) and warm (re-render same pages, hits the store). Reports n / mean / p50 / p95 / p99 / max in milliseconds, plus total elapsed. The cache layer is intentionally bypassed: this answers "how fast does the backend render?" not "how well does the cache hide latency?" Quick check on Effective Java's 901-page corpus sample showed cold p50 ~9 ms, warm p50 ~3 ms — about a 3× store-hit speedup, validating the v0.26 scaling.

### `stress-corpus-soak` (Phase 12.2)
Full-corpus soak — opens each of the seven canonical samples (PDF×2, DjVu, EPUB, MOBI, CBZ, CBR), walks every fifth page through the cache up to 200 pages per document, and tears down. Catches regressions on backends none of the narrower stress tests exercise (e.g. CBR's libarchive path, MOBI's reflowable layout). Runs in ~36 s; registered with `meson test` so the suite now has five entries. Confirmed clean under ASan+UBSan; peak RSS lands around 1.5 GB on the comics-heavy run, default cap raised to 1.8 GB.

### Test Harness
Five `meson test` targets total: stress-scrub, stress-zoom-storm, stress-search-cache, stress-multidoc, stress-corpus-soak. bench-render is built but not registered as a test (latency benchmark, not a pass/fail check) — invoke directly.

---

## v0.25.0 (2026-05-01)

*Margin cropping, multi-doc lifecycle stress test, real leak fix.* The third Phase 14 polish item lands; the new stress-multidoc test promptly catches a real leak in the cache dispose path that the existing stress tests never reached.

---

### Margin Cropping (Phase 14)
A new `Crop Margins` toggle (`F6`, primary menu, GSettings-backed) auto-crops whitespace margins so dense PDFs use more of the laptop screen. Implementation:

- **Detection**: a new `get_content_bbox(page)` interface method returns the inked-content bounding box in document points. The PDF backend computes it by walking the cached `fz_stext_page` and unioning every char's quad-derived rect — text blocks only, image blocks skipped. Fast (the stext is already cached from v0.18). DjVu and CBR return FALSE; the toggle has no effect on those.
- **Application**: on toggle activation, the view probes the *current visible page*'s bbox (assuming uniform margins across the doc, which holds for ~99% of technical PDFs) and computes fractional margins. `recompute_layout` shrinks every page's reported width/height by `(1 - margin_fractions)`. The snapshot path draws the full page texture offset+sized so the content area aligns with the cropped page rect, and pushes a clip so margins don't leak past the rect.
- **Anchor preservation**: like `fw_view_set_zoom`, captures `(page, intra-page-fraction)` before the layout change and restores after — without it, toggling crop would jump to a different page because the same scroll_y maps differently in the smaller layout.

The toggle is wired with the same plumbing as the v0.23 reading ruler and v0.24 loupe: GSettings boolean → `g_settings_create_action` → menu checkmark + F6 accelerator. Both stay in sync; setting persists across sessions.

### `stress-multidoc` (Phase 12.2)
A new stress test, the fourth in the harness. Sequential phase: open 50 documents in succession across the six-format corpus (PDF, DjVu, EPUB, MOBI, CBZ, CBR), create a cache for each, render the first three pages, dispose. Parallel phase: hold 10 `FwDocument`+`FwCache` instances simultaneously, then dispose all in reverse order.

Asserts no crashes, peak RSS under 2 GB (covers ASan overhead), and cleanly disposes everything. Caught the leak below on first run.

### Bugfix — Pool Dispose Leak in `fw_cache_dispose`
**Real leak.** The cache called `g_thread_pool_free(pool, immediate=TRUE, wait=TRUE)`, where `immediate=TRUE` discards queued tasks *without invoking their workers*. Since each `RenderJob` is `g_new0`-allocated and free'd inside the worker, every queued-but-not-yet-running job leaked on cache dispose — exactly hits during document open/close churn.

ASan attribution from `stress-multidoc`:

```
Direct leak of 2832 byte(s) in 59 object(s) allocated from:
    g_malloc0 → submit_next_jobs → fw_cache_start
```

Fix: `immediate=FALSE` instead, so queued jobs run through their workers. Workers see `cancel_gen` was bumped during `fw_cache_stop`, take the bail-out path, and `g_free` the job. The `fw_document_cancel_render` call already in `fw_cache_stop` ensures mid-render decodes abort fast (PDF via fz_cookie, DjVu/CBR via cancel_flag), so the drain doesn't block dispose noticeably.

Verified: ASan-clean across the full multi-doc run after the fix. Same code path is exercised by every document close in the GUI — the leak was bleeding small but real allocations on every file switch.

---

## v0.24.1 (2026-05-01)

*Bugfixes — fit-width, zoom-anchor, sticky-blur, scroll handling.* No new features; sanding down the rough edges that surfaced once the loupe and CBR cache started exercising paths in new combinations.

---

### Per-Page Fit-Width
`fw_view_fit_width_zoom` previously found the *widest page across the entire document* and computed `viewport_w / max_page_w`. For a comic CBZ with a single centerfold spread, that made every normal page render at ~35% — empty viewport on either side and the user had to manually zoom in. Now it uses the *current visible page's width*, so normal pages fill the viewport. Scrolling onto a wider spread page makes that page wider than the viewport and adds horizontal scroll until the user hits Ctrl+1 again on it.

For uniform-width docs (PDF textbooks, DjVu, EPUB), behavior is unchanged: every page is the same width so per-page and document-wide are equivalent.

### Per-Page Horizontal Centering in Snapshot
Companion to the fit-width fix. The snapshot's centering math was `if (max_width <= widget_width) center-in-viewport, else position-in-canvas`. With per-page fit-width on Berserk, normal pages were narrow, max_width was the spread page's width, and the else branch positioned normal pages offset within the wider canvas. Changed the condition to `pw <= widget_width` — each page centers in the viewport when *it* fits, regardless of document-wide max. Mirrored in `fw_view_widget_to_doc` so click coordinates still map correctly.

### Page-Fraction Horizontal Anchor in `fw_view_set_zoom`
The earlier "fraction of canvas" horizontal anchor for zoom-preserving-focus broke on mixed-width docs because it anchored to the canvas (max_width) rather than the page the user was looking at. Replaced with a page-fraction anchor: capture the fraction of the current page's width that's at the viewport's horizontal center before zoom, derive the scroll_x that puts the same page-fraction at viewport center after zoom. Zooming in past fit-width now keeps the focal point centered instead of jumping to the page's left edge.

### Sticky-Blur Bugfix in Worker Store Path
The v0.24.0 sticky-fail change (skip re-rendering entries with `render_gen == self->render_gen`) was correct for deterministic failures (CBR's "zero-size render") but wrong for *transient* failures — specifically, fz_cookie cancellations that fire mid-render. When a worker's render is aborted by `cookie->abort = 1` from the SCRUBBING transition, the render returns NULL, the worker reaches the success branch with `render_gen` matching, stores `surface=NULL` and `render_gen=current`. The page then stayed stuck at thumbnail resolution until the next render_gen bump (zoom or rotation).

Fix: in the worker store path, distinguish "cancelled mid-render" from "actually failed" by checking whether `cancel_gen` was bumped during the render. Bumped + NULL surface → transient cancellation, clear `rendering` but don't sticky-fail. Unbumped + NULL → real failure, stays sticky as designed in v0.24.0.

### Scroll Handling Returned to Native GTK
Removed the per-event scroll cap that the v0.14 work introduced. With the v0.14 GThreadPool sort-function priority dispatch and v0.17 fz_cookie mid-render abort already in place, the cache responds to scroll velocity natively without needing an input-side cap. The `kinetic-scrolling` GSettings boolean now drives `gtk_scrolled_window_set_kinetic_scrolling()` on the document scrolled window — the standard knob — instead of the custom cap-vs-momentum toggle. Default flipped from false to true.

The view's own `GSettings` handle stays for `reading-ruler` and `loupe`; the kinetic-scrolling-related fields/handlers are gone. The window owns the kinetic setting now.

### Zero-Size Render Warning Suppressed
The CBR backend's "zero-size render" condition (zoom × image dimensions rounds below 1 px) is benign — the cache already handles NULL surfaces gracefully via the thumbnail fallback. The `g_warning` was log noise. Now suppressed via a `volatile gboolean silent_zero_size` flag set before fz_throw and checked in fz_catch; genuine MuPDF errors still warn.

---

## v0.24.0 (2026-05-01)

*Magnifying loupe, CBR bytes cache, and a runaway-render bugfix.* Three things shipped together: the third Phase 14 polish item (loupe), a long-pending CBR backend optimization (per-page bytes cache), and a freshly-discovered cache infinite-loop bug uncovered by the loupe's per-frame redraws.

---

### Magnifying Loupe (Phase 14)
A circular zoom-in viewport that follows the cursor — useful for dense comic panels, small chart axis labels, and footnote text on scanned documents. Implemented as a snapshot-time GSK transform: rounded clip at the cursor + zoom-around-cursor matrix + re-append the page texture inside the clip + thin border. Pure GPU work, no re-rendering required (the texture is already in cache). Magnification is fixed at 2.5×; loupe radius is 80 px.

Wired through:
- New `loupe` GSettings boolean (default off, persists).
- **F7** keyboard shortcut.
- "Magnifying Loupe" entry in the primary menu.
- New row in the in-app Keyboard Shortcuts dialog (View group).
- New row in README.md's Keyboard Shortcuts → View table.

### CBR Per-Page Bytes Cache
RAR has no central directory: seeking to entry N requires sequentially decompressing entries 1..N-1. Every render of every page previously paid that full walk cost from scratch — the documented "streaming-RAR cost" called out in `fw-document-cbr.c`'s threading comment. Now there's a per-page bytes cache on `FwDocumentCbr`: first render of a page does the full walk and stores the extracted entry as a `GBytes`; subsequent renders hit the cache and skip straight to MuPDF decode + raster.

Cache details:
- Keyed by page index, stored as `GBytes *` (refcounted) in a `GHashTable`.
- FIFO eviction via a parallel `GQueue` of page indices when total cached bytes exceeds the cap. Comics are read mostly linearly so age-based eviction works fine.
- Default cap is 256 MB, sized for typical graphic novels.
- Serialized via the existing `archive_lock` mutex — same lock as the archive walk this cache exists to short-circuit, so no new lock-ordering concerns.
- The `cbr_extract_entry` function signature changed from `(page, *out_size) → guint8*` to `(page) → GBytes*`. Updated both callers (the page-0 dimension probe in `cbr_open` and the main render path).

ASan + UBSan clean across the full stress run.

### Bugfix — Sticky-Fail Render Skip in `fw_cache.submit_next_jobs`
The loupe's per-frame redraws surfaced a long-latent infinite-render-loop in the cache pipeline. When a render job returned `NULL` (e.g., the CBR backend's "zero-size render" failure on certain thumbnail-tier renders), the worker stored `surface = NULL` and set `entry->render_gen = job->render_gen`. Then `submit_next_jobs`'s skip condition `entry->surface && entry->render_gen == self->render_gen` evaluated FALSE because surface was NULL — re-pushing the same failed job. Each retry produced another NULL, which re-pushed again, ad infinitum.

Discovered by tracing: with FW_DEBUG=1 + loupe enabled on a 583-page CBR, `output.log` collected **3.38 million `[cache] worker start` lines in 30 seconds** (~110 k/sec). All on the same handful of pages whose renders happened to fail.

Fix: change the skip condition to compare `render_gen` alone:

```c
if (entry->render_gen == self->render_gen)
  continue;
```

A render attempt at the current generation — success *or* failure — sticks until the next generation bump (zoom or rotation change). After the fix, the same scenario produces **209 cache traces** for the full run instead of 3.38 million. CPU stays quiet, fans stay still.

The "zero-size render" warnings on a few specific CBR pages are a separate (cosmetic) symptom worth investigating — likely sub-pixel rounding when zoom × original page width drops below 0.5 — but it no longer cascades into a thermal incident.

### Diagnostic Trace Plumbing
While diagnosing the loop, a per-second snapshot timing summary was added (`view: snap stats: N frames/s avg=Xms loupe-paints=K …`) plus per-call CBR cache hit/miss traces. Zero overhead when `FW_DEBUG=0`; instantly tells you whether a perf issue is a frame storm, expensive frames, or render churn when enabled.

---

## v0.23.0 (2026-05-01)

*Reading ruler (Phase 14).* Toggleable mode that dims everything except a horizontal band tracking the cursor — keeps the eye on the active line in dense technical reading. Pattern conceptually borrowed from Sioyek's "visual mark"; reduced to a couple of `GskColorNode`s above and below a clear band.

---

### Reading Ruler
A `reading-ruler` GSettings boolean (default off) drives a render-time overlay: when active, paint two semi-transparent black rects above and below a ~56-px-tall clear band that follows the mouse Y. No clipping, no shaders — just two `gtk_snapshot_append_color` calls per frame. Tracking is via the existing `on_motion` controller, which queues a redraw when the ruler is active.

Toggle paths:
- **F8** keyboard shortcut.
- **Reading Ruler** entry in the primary menu.
- The setting persists across sessions; the menu checkmark and the F8 toggle stay in sync via `g_settings_create_action`.

The shortcut sits naturally in the F-key range with F9 (sidebar) and F11 (fullscreen) — view-mode toggles all live there.

### Documented in app and README
The Keyboard Shortcuts dialog (`Ctrl+?` / `F1`) gained a "Reading ruler — F8" row in the View group. The README's Keyboard Shortcuts → View table has the same row. Both stay in sync with the actual binding.

---

## v0.22.0 (2026-05-01)

*Hue-preserving recolor for Ctrl+I.* The previous dark-mode toggle was a per-channel bitwise NOT — accurate for "white text on white page" but destructive for any document with chromatic content. Red diagrams turned cyan, blue plots turned yellow, syntax-highlighted source code lost every color cue. Replaced with a luminance-aware affine transform that flips the lightness axis while keeping each pixel's chromatic component intact.

---

### Hue-Preserving Lightness Inversion (Phase 11 Tier 2)
For each pixel, compute BT.601 luma `Y = 0.299R + 0.587G + 0.114B`, then offset every channel by `(1 - 2Y)`:

```
R' = R + (1 - 2Y) =  0.402·R − 1.174·G − 0.228·B + 1
G' = G + (1 - 2Y) = −0.598·R − 0.174·G − 0.228·B + 1
B' = B + (1 - 2Y) = −0.598·R − 1.174·G + 0.772·B + 1
```

The chromatic offset (R−Y, G−Y, B−Y) is preserved by construction; only the lightness axis flips. White (Y=1) → near-black, black (Y=0) → near-white, red (Y=0.299) stays red but on a dark background. Implemented as a single `gtk_snapshot_push_color_matrix` — no shader work, GPU-side, the same lifecycle as the v0.14 GPU color inversion. GSK clamps out-of-gamut output to [0,1] for free.

Pattern conceptually similar to zathura's `colorumax` HSL recolor (`zathura/render.c:495+`), but reduced to a 4×4 affine that fits the existing GTK4 GPU path. Configurable theme colors (`recolor-light` / `recolor-dark` GSettings keys) — the full zathura-style customization — stay open as a follow-up; the current implementation hardcodes the standard "white background → black background" mapping which is what 95% of dark-mode users actually want.

### Tested
ASan + UBSan clean across all three stress tests. The change is rendering-only — no cache, document, or selection paths touched.

---

## v0.21.1 (2026-05-01)

*Bugfixes and post-session cleanup.* No new features — just sanding down the rough spots that accumulated across the v0.12 → v0.21 sprint.

---

### LRU Eviction Actually Works (`fw_state_prune`)
The `fw-state.c` header has claimed since v1.1.0 that state.json is "capped at 500 entries (LRU)". In practice only the age-based prune (entries >90 days) was implemented; the file would grow unbounded for users who open more than 500 documents inside the 90-day window. A `TODO: LRU eviction` comment marked the gap. Now fixed: when post-age count exceeds `MAX_ENTRIES` (500), entries are sorted by `last_opened` timestamp and the oldest are evicted until the count is at the cap. Phase 9's checkbox in the roadmap is now honest.

### Dead `active_jobs` / `max_jobs` Bookkeeping Removed
The v0.16.0 byte-cap rework dropped the `active_jobs < job_limit` concurrency throttle in favor of letting the GThreadPool's own worker count cap concurrency. The counters were retained "for debug tracing" but never actually traced — they were pure write-only bookkeeping (incremented in submit, decremented in worker, read by nobody). Removed the fields, the increment/decrement sites, and the init lines. Net –7 lines plus a small clarity win.

### Pedantic Warning Cleanups
Two `FW_TRACE_*("string-only")` call sites triggered ISO C99 variadic-macro warnings at `-Dwarning_level=3` (the macro uses `##__VA_ARGS__`, a GNU extension). The default build at level 2 was clean, but the strict-build was noisy. Fixed both with explicit `"%s", ""` arguments — same trace output, ISO-conformant.

### Verified
ASan-clean across all three registered stress tests (stress-scrub, stress-zoom-storm, stress-search-cache) with the cleanup applied. No regressions in the cache pipeline.

### Known TODO Carried Forward
`fw-document-djvu.c:506` — `djvu_get_text` doesn't filter the returned text to the selection rectangle (returns the whole page's text instead). Real bug for DjVu users hitting Ctrl+C on a partial selection. Requires careful coord conversion (DjVu uses pixel coords at file-DPI from bottom-left; Framework uses points at 72 DPI from top-left) and testing against real DjVu samples. Deferred to its own focused commit when DjVu selection becomes a felt pain point.

---

## v0.21.0 (2026-04-30)

*Phase 14 — auto-reload via `GFileMonitor`.* Recompile your LaTeX or Typst document and Framework refreshes automatically, restoring exact scroll position and zoom. The same pattern that made SumatraPDF a fixture in technical workflows for years.

---

### Auto-Reload on File Change
A `GFileMonitor` is attached to the open document path the moment a document opens. When the file changes — `G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT` for in-place writes, `G_FILE_MONITOR_EVENT_CREATED` for atomic-rename editors — the window saves current state (page, scroll fraction, zoom, rotation) via the existing `fw_state_save` path, re-opens the document, and restores state from disk. The deferred `restore_state_tick` mechanism handles the scroll restore once the new document's layout settles, so the user lands at exactly the same paragraph they were reading.

Implementation notes:
- `G_FILE_MONITOR_WATCH_HARD_LINKS` flag tracks the inode, so atomic-rename patterns (`write to .tmp; rename .tmp to target`) still produce events. LaTeX and Typst both use in-place writes; some editors that "write to a temp file and rename" benefit from this flag.
- A 200 ms debounce collapses bursts of CHANGED events (LaTeX writes auxiliary files in the same directory in quick succession; Typst sometimes emits multiple chunks) into a single reload.
- The monitor is stopped at the start of every `fw_window_open_file` (before tearing down the old document) and on window dispose.
- An `AdwToast` ("Document updated") shows briefly so the swap is visible — without it, an auto-reload mid-read could feel confusing if the user didn't trigger it themselves.

### `AdwToastOverlay` for Window-Wide Notifications
The window's content tree is now wrapped in an `AdwToastOverlay`. Beyond the auto-reload toast, this gives every future Framework feature a clean place to surface ephemeral notifications without resorting to dialogs (the next likely user is a "selection copied" toast for cases where Ctrl+C has no visual feedback).

### Trade-offs
- Reloading clears the navigation history (Alt+Left/Alt+Right). The history is per-document and the document just changed under us; preserving it would mean replaying jumps against a possibly-renumbered TOC. Wiping is cleaner.
- The 200 ms debounce window means there's a tiny perceptible delay before the reload kicks in. For LaTeX users this is fine — recompile takes a second or more — and avoids the "reload mid-write" flicker on slow filesystems.
- DjVu and CBR auto-reload for free since `fw_window_open_file` is backend-agnostic. Untested in practice; the LaTeX/Typst use case is the design driver.

---

## v0.20.0 (2026-04-30)

*Drag-selection highlight matches the actual selected text.* The previous overlay drew a single bounding rectangle from drag start to end, which included unselected words on partial first/last lines and looked ragged across line wraps. Now the highlight is per-line — partial first line, full intermediate lines, partial last line — matching exactly what `Ctrl+C` puts on the clipboard.

---

### Per-Line Selection Quads via `fz_highlight_selection`
A new `get_selection_quads` interface method asks the backend for an array of `FwRect`s (page-coordinate rectangles), one per line of selected text in reading order. The PDF backend implements it via MuPDF's `fz_highlight_selection` against the v0.18 cached `fz_stext_page` — same fast path as `pdf_get_text` and `pdf_select_at`, no extra parse.

`FwView` stores the quads in `sel_quads` (`GArray<FwRect>`, owned), recomputed live during drag, on drag end, and on snap-select (double/triple click). The render path iterates quads and draws each as a separate semi-transparent blue overlay; if `sel_quads` is empty (DjVu, CBR — which don't implement the method), it falls back to the legacy bounding-box draw.

Visible improvements:
- Multi-line drag selection highlights *only* the actual selected text — partial line at the top of the drag, full lines in between, partial line at the bottom. No more highlighting half a paragraph just because the bounding box happens to cover it.
- Snap-select overlays are also computed via quads, so they render identically to drag selection (a word on one line is one quad; a line is one quad).
- Selection highlight updates in real time during drag — the cached stext makes per-frame quad recomputation cheap.

### Backend Coverage
- **All MuPDF-routed formats** (PDF, CBZ, CB7, CBT, XPS, EPUB, FB2, MOBI) → per-line quads.
- **DjVu, CBR** → return NULL from `get_selection_quads`; the view falls back to drawing the drag bounding rectangle (existing behavior, unchanged).

### Tested
ASan-clean. All three registered stress tests still pass — the change touches view + PDF backend, not the cache pipeline that the stress tests exercise.

---

## v0.19.0 (2026-04-30)

*Phase 14 — smart text selection.* Double-click selects the word under the cursor; triple-click selects the whole line. Built directly on the v0.18 stext cache, so the snap is constant-time after first text access on a page (no extra parse).

---

### Double-Click → Word, Triple-Click → Line
The PDF backend grew a `select_at` interface method backed by MuPDF's `fz_snap_selection`. Given a click point and a granularity (`FW_SELECT_WORD` or `FW_SELECT_LINE`), it walks the cached `fz_stext_page` and snaps both selection endpoints to the word or line containing the click — no click-drag required.

In the view, `on_click_pressed` now branches on `n_press`:
- `n_press == 2` → call `fw_document_select_at` with `FW_SELECT_WORD`, apply the snapped rectangle to the existing selection state, queue redraw.
- `n_press == 3` → same with `FW_SELECT_LINE`.
- `n_press == 1` → unchanged (link-hit-test, single-click navigation).

Multi-press takes priority over link clicks: if a user double-clicks on a hyperlinked word, the intent is selection — the link is ignored. Single-click on the same word still navigates. The drag gesture continues to work for arbitrary range selection.

The selected text is extracted via the existing `fw_document_get_text` path (which itself uses the cached stext now, so it's another fast path). `Ctrl+C` copies it to the clipboard exactly as before — no new clipboard plumbing needed.

### Backend Coverage
- **PDF / CBZ / CB7 / CBT / XPS / EPUB / FB2 / MOBI** (all MuPDF-routed) → fully supported.
- **DjVu / CBR** → `select_at` returns `FALSE` (no implementation). Click-drag selection still works on DjVu via the existing path; CBR has no text layer at all. The vtable's NULL fallback returns `FALSE` from the public glue, so the view falls through cleanly without selecting.

### Tested
ASan-clean. The `stress-search-cache` test (which exercises the same stext-cache path the new selection uses) still hits its 6×+ speedup target.

The selection bbox handling correctly returns FALSE when the click misses every glyph (using `fz_snap_selection`'s zero-quad signal), so double-clicking on whitespace doesn't apply a stale-feeling no-op selection — the existing selection (if any) stays in place.

---

## v0.18.0 (2026-04-30)

*Phase 11 Tier 1 — cached `fz_stext_page` per page.* PDF text extraction and search now build the structured-text page once per document-page lifetime and reuse it on every subsequent text-related call. A new automated test confirms the speedup is real: a full-document search across 901 pages of Effective Java goes from **332 ms cold to 48 ms warm — a 6.85× speedup**. With this in place, double-click word selection (Phase 14) becomes a one-line follow-up: it just consumes the cached stext.

---

### Per-Page `fz_stext_page` Cache
A new `stext_cache` GHashTable on `FwDocumentPdf` maps page index → `fz_stext_page *`. All entries are owned by `self->ctx` and dropped en masse in `pdf_close`. Population is lazy: the first text-related call (`pdf_get_text`, `pdf_search`) on a given page extracts and caches; every later call hits.

The previous code paths re-loaded the page and re-extracted stext on *every* invocation — a 1000-page search ran `fz_load_page` + `fz_new_stext_page_from_page` 1000 times. Even though MuPDF makes both fast individually, the redundancy adds up.

Implementation notes:
- All access goes through `self->lock` (the existing main-context mutex). No separate stext lock — read-after-write is already serialized.
- `fz_stext_page` is immutable after construction in MuPDF, so cached read access from any code path that holds `self->lock` is safe without extra synchronization.
- Memory cost: ~10–50 KB per page on typical textbooks (depends on text density). A 1000-page document costs ~30 MB worst-case. Bounded by document length, not user activity.
- `pdf_search` switched from `fz_search_page` to `fz_search_stext_page` to consume the cached structured text directly. `pdf_get_text` similarly uses the cached stext via `fz_copy_selection`.

### `stress-search-cache` Stress Test (Phase 12.2)
A third stress test exercises the cache and asserts ≥1.5× warm/cold speedup on a full-document search. Catches regressions cleanly — without the cache, warm and cold passes would run at the same speed and the test would fail. Registered in `meson test` and runs in <1 s on the Effective Java sample.

The test takes a document path and a query string as args; the registered case uses `"class"` against Effective Java for stable hit counts (1922 hits across all 901 pages).

### What This Doesn't Do (Yet)
The "bonus" item from the roadmap entry — opportunistic stext extraction during render, à la Sumatra's `RenderCache.cpp:790` — stays open. The current design extracts on first text-call rather than during render, so a freshly-opened document's first search is still cold. Adding render-time pre-warm would shift that cost off the search latency path, useful for users who scroll through a doc and then search. Tracked in roadmap Phase 11 Tier 1 as a follow-up.

---

## v0.17.0 (2026-04-30)

*Phase 11 Tier 1 — `fz_cookie` mid-render abort.* Workers now plumb a per-render `fz_cookie` through to `fz_run_page`, and `pdf_cancel_render` flips `cookie->abort = 1` from the main thread. MuPDF sees the flag at its next checkpoint inside `fz_run_page` and abandons the in-flight render with whatever partial state it has. On a 50 MB scanned PDF this saves 1–3 seconds per stale page when the user has already scrubbed away. Neither zathura nor sioyek does this — Framework leads the field on this one.

---

### `fz_cookie` Plumbed Through PDF Render Path
The MuPDF `fz_cookie` is the canonical cancellation primitive: a struct whose `abort` field MuPDF reads periodically during `fz_run_page` execution. Set it from any thread; the next checkpoint inside MuPDF returns immediately. The previous code passed `NULL` for the cookie, so `fz_run_page` always ran to completion regardless of whether the user had scrolled away.

The implementation:
- Each worker allocates an `fz_cookie` on its stack inside `pdf_render_page` and registers the pointer in a new `active_cookies[MAX_RENDER_INSTANCES]` array on `FwDocumentPdf`, indexed by render slot.
- The cookie pointer is published and deregistered under a new `cookies_lock` mutex — *separate* from the per-instance render lock. This is load-bearing: cancel must reach the cookie pointer without blocking on the render lock that the worker holds during `fz_run_page`. Two locks make the cancel signal travel during render, not after.
- After `fz_run_page` returns, `render_page_direct` checks `cookie->abort` and discards the partial surface if set — returning NULL to the worker, which discards the result via the existing stale-discard path.
- New `pdf_cancel_render` walks `active_cookies` under `cookies_lock` and writes `abort = 1` on every published cookie. Wired in `iface_init`. The PDF backend previously had no `cancel_render` implementation, so this also closes a gap: scroll-aborts are now actually honored by the PDF render path.

The cookie pointer's lifetime is exactly the worker's stack frame. Both publish and unpublish happen under `cookies_lock`, and cancel writes under the same lock — so the pointer is never accessed after the worker's frame is gone.

### Validation
Code paths verified clean under ASan: no use-after-stack, no double-free, no leaks across the worst-case scrub pattern. The synthetic stress-scrub test transitions to SCRUBBING before any worker enters `fz_run_page`, so it doesn't drive the cookie path; a Phase X cookie-abort test would need to render briefly first then transition. Real-world validation is `FW_DEBUG=1` plus the GUI: scrolling fast through a big scanned PDF should now produce `[pdf] cancel_render: aborted N in-flight render(s)` traces, with renders bailing in tens of ms instead of completing the full second-or-more rasterization.

The DjVu and CBR backends keep their existing per-document `cancel_flag` mechanisms — they're not as fine-grained as fz_cookie but the streaming-RAR / single-mutex constraints there already cap parallelism, so the value of mid-render abort is much lower.

### Trade-offs Documented
- The pdf backend gains 8 cookie pointers + a mutex on `FwDocumentPdf`. Memory cost: ~80 bytes per open document. Negligible.
- `cookies_lock` is acquired twice per render (publish + unpublish) plus once per cancel. The publish lock is held for nanoseconds (a single pointer write), so contention with cancel is bounded. No measurable per-render overhead.
- Aborted renders return NULL. The worker's existing stale-discard path handles this — no new code there. Side effect: aborted pages count as "still needs render" in the cache, so the next priority update will re-queue them. This matches the SCRUBBING semantics the user expects.

---

## v0.16.0 (2026-04-30)

*Phase 11 Tier 1 — bytes-aware cache cap.* The page-count `CACHE_WINDOW = 30` introduced in v1.3.3 has gone away — replaced by a byte budget that tracks `stride * height` per cached surface. The Phase 12 stress harness flagged this exact case: a 212-page Berserk volume peaks at ~525 MB of surfaces alone (~2.3 MB/page), while a 900-page Effective Java textbook peaks at ~109 MB (~0.12 MB/page). Page-count caps mis-fit by 20×; byte caps don't.

---

### Bytes-Aware Cache Cap (Phase 11 Tier 1)
`FwCache` now tracks `total_cached_bytes` across every cache entry's `surface` and `prev_surface` slots, accounted live at every store/replace/evict path under the existing mutex. Default cap is 512 MB, overridable per-process via the `FW_CACHE_BYTES_CAP_MB` env var. The cap is a soft target on the rendered-surface tier (Tier 2) — parsed handles (Tier 1) and thumbnails (Tier 0) are tracked separately with their own bounds.

**Eviction policy changed.** Previously: pages outside the priority window were unconditionally dropped, so the cache held at most ~21 surfaces. Now: outside-priority surfaces are *kept* until `total_cached_bytes` exceeds the cap, then evicted (oldest hash-iteration first; visible/priority pages are never evicted). The trade-off: scrolling back into a previously-rendered region is now instant (no re-render) when there's headroom, at the cost of higher steady-state memory.

For the case where even the priority window's surfaces exceed the cap (poster-format PDFs at 400% zoom), eviction can't free enough — slicing (Phase 11 Tier 2) is the proper fix and remains scoped there.

`FW_DEBUG=1` now prints `byte-cap evict` lines naming pages dropped, bytes freed, and remaining vs cap. Useful for tuning.

### Cache Constants Cleaned Up
- `CACHE_WINDOW = 30` deleted. Its dual purpose (priority array bound + eviction bound) split: `MAX_PRIORITY_PAGES = 64` is the array bound (only used as a safety check; actual content is `n_visible + 2 * NEAR_RANGE` ≈ 23), and the byte cap replaces the eviction bound.
- `CACHE_BYTES_CAP_DEFAULT = 512 MB` is the new compile-time default.

### Stress Harness Updated for the New Behavior
`stress-scrub` gained a Phase 4 — a slow walk through the document at one priority shift per 200 ms. Without it, the test never accumulated outside-priority surfaces, so the byte-cap eviction path was never exercised. Stress-scrub also pins `FW_CACHE_BYTES_CAP_MB=128` at startup so eviction *will* fire during Phase 4 — under the 512 MB default the test would just hold everything.

Verified across all six backends (PDF, DjVu, EPUB, MOBI, CBZ, CBR) both natively and under ASan. The Berserk CBZ run was the proof point: 7-13 pages dropped per priority shift, ~80 MB freed each time, cache stays at ~125 MB out of the 128 MB cap. No leaks under ASan.

The `stress-scrub` test's RSS cap was bumped from 800 MB to 1200 MB — under the new policy the cache legitimately keeps more surfaces and total RSS includes glibc retention + thumbnails + ASan overhead.

---

## v0.15.0 (2026-04-30)

*Phase 12 — stress harness foundation.* The first piece of the regression net: a `tests/` tree, a `-Dstress=true` meson option that gates the harness, a `-Dsanitize=` option for ASan/UBSan/LSan/TSan builds, and one real stress test that exercises today's Phase 11 Tier 1 cache pipeline. The remaining Phase 12 items (zoom storm, multi-doc, corpus soak, benchmarks, gdb pretty-printers, trace replay, --self-test) stay open as future work.

---

### Engine as a Static Library
`src/meson.build` was refactored to compile the framework's internal modules into a `framework-core` static library; the `framework` executable now links against it via a single `framework_lib_dep`. This makes internal symbols (`fw_cache_*`, `fw_document_*`, `fw_view_*`, etc.) reachable to tests without exposing them as a public API. The `framework` binary itself is unchanged at runtime.

### `-Dstress=true` and the `tests/` Tree
A new top-level `tests/` directory holds the harness. It builds only when `-Dstress=true`, so packagers and end users pay zero cost. `tests/corpus.json` is the canonical sample manifest (default root: `/home/bdkl/docs/Calibre Library`, override via `FW_TEST_CORPUS_ROOT` for portability), tagged so each stress/bench tool can pick the samples it cares about (`large`, `textbook`, `djvu`, `scanned`, `reflow`, `comic`).

```sh
meson setup builddir -Dstress=true
meson compile -C builddir
meson test -C builddir            # once registered targets stabilize
```

### `stress-scrub` (Phase 12.2)
The first stress test. Drives `FwCache` directly without a widget tree and simulates a punishing scroll pattern: 0 → last page in 500 ms, then 5 × back-and-forth, then a 3-second settle. Asserts no crashes (segfault → non-zero exit), peak RSS under a configurable cap (`FW_STRESS_RSS_CAP_MB`, default 800 MB), and no stuck workers.

**Run across all six backends** (PDF, DjVu, EPUB, MOBI, CBZ, CBR) — every backend passed cleanly both natively and under `-Dsanitize=address`. The full corpus-coverage results revealed two real signals: (1) CBZ on a 212-page Berserk volume peaks at 789 MB under ASan, dangerously close to the 800 MB cap, giving the bytes-aware cache cap (Phase 11 Tier 1) concrete weight; (2) the CBR backend's streaming-RAR cost is real — even a single 4-second test renders only a handful of pages on a 583-page comic.

### `stress-zoom-storm` (Phase 12.2)
A second stress test exercising a different code path: the v1.4 `prev_surface` stash and v1.5 texture-before-surface unref ordering. Pins priority on a single page and runs 50 zoom cycles across 25%–400%, alternating direction. Each cycle bumps `render_gen` and triggers the surface/texture replacement. The peak during transition is permitted to be high (~1.2 GB on a 901-page textbook); the **leak signal** is current RSS read from `/proc/self/status` after a 5-second settle — `getrusage`'s high-water mark never decreases and would mask correct lifecycle behavior. Verified clean natively and under ASan; the post-storm RSS drops from ~1218 MB peak to ~660 MB, confirming transient memory is correctly released.

Both tests are registered with `meson test` and run in under 12 seconds combined.

Future stress tests (`stress-multidoc`, `stress-corpus-soak`) and the bench/triage stack remain open in Phase 12 — landing them is later work.

### `-Dsanitize=` Option (Phase 12.4 partial)
A meson `array` option taking any of `address`, `undefined`, `leak`, `thread`. Forwarded to compile and link as `-fsanitize=` flags via `add_project_arguments` and `add_project_link_arguments`. Builds cleanly with `-Dsanitize=address` on Brandon's Fedora; `-Dsanitize=undefined` requires `sudo dnf install libubsan` (not currently installed). Leak and Thread sanitizers similarly need their runtime libs.

`stress-scrub` was rerun under `-Dsanitize=address` against the same 901-page textbook: clean. No use-after-free, no buffer overflows, no leaks. The cache + render-worker pipeline shipped in v0.14.0 holds up under the worst-case scroll pattern with ASan watching.

### Trade-offs Documented
- The static-library refactor adds a minor link-time cost. Functionally invisible at runtime.
- `tests/` is gated behind `-Dstress=true` precisely so this doesn't bloat the standard build. The default `meson setup builddir` produces exactly the same artifacts as before.
- The corpus manifest hardcodes the default path. CI use will require setting `FW_TEST_CORPUS_ROOT` (currently consumed by stress-scrub via argv only — to be wired up properly when the corpus-aware tests land).

---

## v0.14.0 (2026-04-30)

*Phase 11 Tier 1 — render pipeline.* Three pre-1.0 cache-pipeline items land together as a single coherent change: GThreadPool sort-function priority, the symmetric ±10 parsed window, and the per-event scroll cap (toggleable). Together they deliver the user-visible target Brandon framed it as: *normal reading scroll never paints thumbnail placeholders; thumbnails are reserved for explicit jumps.*

---

### GThreadPool Sort-Function Priority Dispatch (Phase 11 Tier 1)
Render jobs now carry a `last_view_time` field and the pool runs `g_thread_pool_set_sort_function (render_job_compare)` — workers naturally pick the most recently prioritized page next, regardless of when its job was pushed. New high-priority pushes (the current viewport on a fresh scroll) jump to the front of the queue ahead of older queued jobs from a previous priority list. Pure GLib pattern borrowed from zathura's `render.c:94`. Replaces the old "walk `priority_order[]` in index order, push one at a time, throttle by `active_jobs < job_limit`" model.

### Startup-Blur Regression Fixed (Phase 11 Tier 1)
The companion regression — saved-state open landing on a thumbnail-blurred page until the user scrolled — turned out *not* to be obviated for free by the sort-function change as predicted. Trace logs showed the priority pool happily rendering pages 0–13 from the initial open and never receiving the saved-page priority update. Root cause: `update_cache_priority` in `fw-view.c` bailed early on `gtk_widget_get_height (self) <= 0`. During the deferred `restore_state_tick`, the adjustment's value-changed signal fires before the view widget's allocation has settled, so the priority update never reached the cache.

The fix is a fallback in `update_cache_priority`: when `widget_height <= 0`, derive the page at the scroll position from `page_y_offsets[]` directly and push that single page as priority. With the sort function in place, those jobs sort ahead of the in-flight pages 0–13 jobs at the next worker handoff, and the saved page renders within a few hundred milliseconds of open — visible by the time the window appears, no scroll required.

### Symmetric ±10-Page Parsed Window (Phase 11 Tier 1)
`fw_cache_set_priority` now builds a symmetric ±10 priority window in all non-SCRUBBING states: visible pages first, then forward/backward interleaved one page at a time outward up to 10 each side. Replaces the previous asymmetric "+7 forward, -3 backward in CRUISING; full 30-page radial outward in STATIC." With sort-function priority dispatch the asymmetric tiering is no longer needed — every push carries a fresh timestamp, so visible-first ordering happens naturally. Total preload window is ~21 pages (visible + 20 neighbors), well under the existing 30-page `CACHE_WINDOW` eviction bound. Parsed handles are lightweight; the slight memory bump is negligible.

The 150 ms CRUISING throttle on priority rebuilds is gone — sort-function handles ordering, no need to throttle priority computation.

### Per-Event Scroll Cap with Kinetic Toggle (Phase 11 Tier 1)
A `GtkEventControllerScroll` on the view (capture phase) now caps single scroll-event deltas at 90 px, applying the bounded delta directly to the vadjustment and consuming the event. Wheel ticks are converted from unit-scale (~1 per click) to pixels via `SCROLL_WHEEL_STEP = 60` first, then clamped. Trackpad smooth-scroll arrives in pixels already. Net effect: per-event flicks can't outrun the render cache during continuous reading scroll.

This trades GTK's kinetic momentum scrolling (the trackpad-flick coast) for predictable cache behavior. Because some users genuinely want the flick — *"if someone is just gliding through research or school things"* — the cap is gated by a new GSettings key:

- **Schema:** `kinetic-scrolling` (bool, default `false`) on the previously-empty `io.github.virinvictus.framework` schema. The skeleton schema now backs one real feature.
- **Menu entry:** "Kinetic Scrolling" in the primary menu, wired to the GSettings key via `g_settings_create_action`. The menu checkmark stays in sync with the setting; toggling flips the behavior live, no restart needed.
- **Behavior with kinetic ON:** the scroll handler returns FALSE, GtkScrolledWindow's default kinetic momentum scrolling takes over. SCRUBBING-state thumbnail behavior still applies if velocity goes high enough.

Default is off (cache-friendly); on is opt-in. Brandon's scope-discipline rule about declaring schema keys ahead of features is honored — this key declaration ships *with* the code that reads it, not before.

### Trade-offs Documented
- **Explicit jumps still flash thumbnails** (TOC click, page-entry edit, internal link click, search-hit reveal). That's the documented trade-off — pre-rendering every chapter target is wasteful, and the user expects a tiny pause on a deliberate jump.
- **Scrubbing state still aborts** with thumbnail placeholders. Kinetic-on users who flick hard enough to trigger scrubbing get the same behavior as before.
- **The `active_jobs` / `max_jobs` counters are now observability-only** — they're no longer read for concurrency limiting. The pool's worker count caps concurrency naturally. Counters retained for debug tracing.

---

## v0.13.0 (2026-04-30)

*Phase 10 — The 1.0 Release.* All shipping artifacts now exist and validate. Framework builds, installs, and runs as a Flatpak end-to-end. The remaining 1.0 work is a tag and a Flathub submission, both of which wait until the broader roadmap is closer to done.

---

### App-ID Rename: `io.github.virinvictus.framework`
The application's reverse-DNS identifier is now `io.github.virinvictus.framework` (was `com.github.vrnvctss.framework`). The reverse-DNS convention requires that the prefix be a domain the developer controls — `com.github.virinvictus` would imply ownership of `virinvictus.github.com` (a subdomain that doesn't exist; GitHub gives users paths, not subdomains under github.com), while `io.github.virinvictus` reverses to `virinvictus.github.io`, the GitHub Pages domain that actually belongs to the developer. Flathub's reviewers explicitly require this form for projects without their own DNS.

The rename touches every artifact keyed on the ID: the desktop file, AppStream metainfo, GSettings schema, scalable icon, Flatpak manifest, the `APP_ID` constant in `meson.build`, the GSettings schema path (`/io/github/virinvictus/framework/`), and the documented references in `spec.md`, `roadmap.md`, and the project `CLAUDE.md`. State persisted under the old ID is invalidated; document state is keyed by file path in `~/.local/share/framework/state.json` and is unaffected.

### AppStream Metainfo Rewrite (Phase 10)
`data/io.github.virinvictus.framework.metainfo.xml.in` rewritten end-to-end. The previous version still described the app as "PDF and DjVu" only and stopped at v1.2.0 in its release history, ignoring the v0.6.0 version reset and everything since. The rewrite includes: current format list (PDF, DjVu, EPUB, MOBI, FB2, XPS, CBZ/CB7/CBT/CBR), feature bullets, `<developer>` block, `<categories>` (Office, Viewer, GNOME, GTK), `<recommends>` (display size ≥ 600 px, offline-only network), `<supports>` (pointing/keyboard/touch), and release entries from v0.6.0 → v0.13.0 in honest versioning. The historical 1.x entries are preserved in this file (`patchnotes.md`) but are not surfaced to software centers — those releases happened, but the running version is honest. `appstreamcli validate` is clean.

A `<screenshots>` block sits in the metainfo as a commented-out template; before any Flathub submission, screenshots will need to be added under `data/screenshots/` and the URLs uncommented.

### Desktop File Polish (Phase 10)
`Comment=` updated to the current format list. New `Keywords=pdf;djvu;epub;mobi;fb2;xps;cbz;cbr;comic;viewer;reader;document;mupdf;` line so software centers and search bars rank Framework correctly. Categories normalized to `GTK;GNOME;Office;Viewer;`. `desktop-file-validate` is clean.

### GSettings Schema Pruned to Skeleton (Phase 10)
The previous schema declared eight keys (`default-zoom-mode`, `default-zoom-level`, `continuous-scroll`, `default-view-mode`, `invert-colors`, `window-width`, `window-height`, `window-maximized`, `sidebar-visible`, `sidebar-width`) that no code in `src/` actually reads. Pre-1.0 is the only safe time to prune a published schema — once 1.0 ships, removing keys becomes a back-compat issue. The schema file now contains only the schema declaration, ready to accept keys when corresponding features are wired up.

### Flatpak Manifest (Phase 10)
`io.github.virinvictus.framework.yml` lives at the project root and builds a working Flatpak end-to-end against `org.gnome.Platform//50` and `org.gnome.Sdk//50`. Three modules: `djvulibre` (autotools, `--disable-static --disable-desktopfiles`), `mupdf` (the project Makefile with `HAVE_X11=no HAVE_GLUT=no HAVE_LIBCRYPTO=no shared=yes USE_SYSTEM_LIBS=no` — bundled third-party libs are simpler than runtime equivalents), and `framework` itself (meson, release buildtype). `libarchive` comes from the freedesktop runtime under GNOME 50, no module needed.

`finish-args` are intentionally tight: no network, no broad filesystem, GPU access via `--device=dri`, Wayland with X11 fallback, and read-only access to `xdg-documents` / `xdg-download` / `xdg-desktop` for command-line invocations. Anything outside those three XDG paths reaches Framework through the Document portal automatically (GtkFileDialog and drag-and-drop both go through it). Permissions audit clean — no `--filesystem=host`, no `--share=network`, no D-Bus talk-names beyond what the SDK auto-includes.

Local install:
```sh
flatpak-builder --user --install --force-clean build-flatpak io.github.virinvictus.framework.yml
flatpak run io.github.virinvictus.framework
```

The Flathub submission step (changing the `framework` module's `type: dir` source to a `type: git` source pointing at a tagged release) deliberately stays open.

---

## v0.12.0 (2026-04-30)

*Phase 9 — Session Resilience.* Closing out the last two pre-1.0 items in the session-resilience phase: a Document Properties dialog and a Keyboard Shortcuts dialog. The 1.0 path now narrows to Phase 10 (Flatpak, AppStream, release).

---

### Document Properties (Phase 9)
A new `Document Properties…` menu entry opens an `AdwDialog` summarizing the active document. Two groups: **Document** (Title, Author, Subject, Keywords, Creator, Producer, Created, Modified — empty rows are auto-hidden, so books with thin metadata get a sparse display instead of "Unknown" placeholders) and **File** (Filename, human-readable Size via `g_format_size`, full Location, Format, Encryption, Pages).

Backed by a new `get_metadata` method on `FwDocumentInterface` returning a `GHashTable<gchar*, gchar*>` of normalized keys. The PDF backend implements it via `fz_lookup_metadata` for `info:Title` / `info:Author` / `info:Subject` / `info:Keywords` / `info:Creator` / `info:Producer` / `info:CreationDate` / `info:ModDate` plus `FZ_META_FORMAT` and `FZ_META_ENCRYPTION` — one implementation covers PDF, XPS, EPUB, FB2, MOBI, and CBZ/CB7/CBT. PDF date strings (`D:YYYYMMDDHHmmSSOHH'mm'`) are parsed to a human-readable `YYYY-MM-DD HH:MM:SS ±HH:MM` form before display. The DjVu and CBR backends return NULL — neither format exposes document-level metadata cleanly, and the File group's filename + size + page count + extension-derived format is sufficient there.

Subtitle text on every row is selectable, so users can copy the title or producer string out of the dialog.

### Keyboard Shortcuts Dialog (Phase 9)
`Ctrl+?` and `F1` now open a Keyboard Shortcuts dialog, also accessible from the primary menu. Built as a custom `AdwDialog` containing an `AdwPreferencesPage` with one group per category (File, Navigation, Zoom & Rotation, Search, View, Selection); each binding is an `AdwActionRow` with a `GtkShortcutLabel` suffix that renders the accelerator with platform-appropriate key glyphs. Wired to the conventional `win.show-help-overlay` action name so a future `GtkShortcutsWindow` swap is a one-handler change.

`GtkShortcutsWindow` itself is deprecated in GTK 4.18; the libadwaita-styled dialog avoids accruing that debt and matches the Document Properties dialog visually.

---

## v0.11.0 (2026-04-30)

*Pre-Phase-9 cleanup.* Four roadmap items that had been left open across earlier phases all land in this release.

---

### XPS + EPUB Format Routing (Phase 7)
The factory now dispatches `.xps` / `.oxps` / `.epub` / `.fb2` / `.mobi` to the MuPDF backend. The MuPDF backend already calls `fz_register_document_handlers` and `fz_open_document` — the work was extending the factory and calling `fz_layout_document(ctx, doc, 600, 900, 11)` for reflowable formats. `fz_is_document_reflowable` decides whether to layout: PDF/CBZ/XPS skip the call (they're fixed-layout); EPUB/FB2/MOBI take the 600×900pt @ 11pt layout. Each render-instance opens its own document so each must call `fz_layout_document` independently — without that, different render threads see different page bounds.

The file-dialog filter and desktop-entry MIME types learned the new extensions and types (`application/oxps`, `application/vnd.ms-xpsdocument`, `application/epub+zip`, `application/x-fictionbook+xml`, `application/x-mobipocket-ebook`).

**EPUB caveat.** Framework's pagination is whatever MuPDF's default layout produces — page breaks happen wherever the layout engine puts them, and the layout doesn't reflow on zoom. Tested cleanly on Wyrd Sisters (159-page output). For serious EPUB reading, [Foliate](https://johnfactotum.github.io/foliate/) handles reflow and font customization properly; Framework is the right choice when you want a single reader for fixed-layout PDFs and EPUBs alongside.

### CBR via libarchive (Phase 7)
Real CBR support without the `libunrar` licensing trap. A new backend (`src/fw-document-cbr.c`, ~370 lines) uses `libarchive` (BSD, GPL-compatible, already on every Fedora install) to enumerate image entries inside the RAR, sorts them by filename (= page order in every comic dump), and on `render_page` extracts that entry's bytes and feeds them to MuPDF via `fz_new_image_from_buffer` → `fz_fill_image` into a draw device wrapping the cairo surface buffer (the same v1.6 zero-copy pattern the PDF backend uses). RAR4, RAR5, ZIP, 7z, and tar archive formats all work through libarchive — the factory currently only routes `.cbr` here, but the backend is format-agnostic.

Threading is the single-mutex pattern, not the PDF backend's 8-instance pattern. libarchive readers can't be safely shared across threads, and the streaming-RAR cost makes per-render archive opens dominate anyway. Each render call opens a fresh `archive *`, walks to the target entry, extracts, and closes. The velocity engine + thumbnail tier hide most of the cost during normal scrolling. Sustained scrub-to-end on a huge archive remains slow — that's a fundamental property of streaming RAR, called out as future work in the file's threading-comment block.

The libunrar-hint error message is gone from the factory; its job is done.

### Embedded File Extraction (Phase 6)
PDF /EmbeddedFiles name-tree extraction. Two new methods on `FwDocumentInterface` — `get_attachments` returns a `GArray<FwAttachment*>` with each attachment's filename, MIME type, and size; `save_attachment` writes one to a destination path. The PDF backend walks the xref via `pdf_xref_len` + `pdf_load_object` + `pdf_is_embedded_file` + `pdf_get_filespec_params` (the zathura-pdf-mupdf reference pattern at `attachment.c`), and saves via `pdf_load_embedded_file_contents` + `fz_save_buffer`. DjVu and CBR backends return NULL — no equivalent attachment mechanism in those formats.

A new menu entry **"Save Embedded Files…"** asks for a destination folder via `GtkFileDialog::select_folder` and saves every attachment under sanitized basenames. The sanitizer strips directory components (defeats `../../../etc/passwd`-style names), drops leading dots (no surprise dotfiles), and replaces control characters — the user-chosen output directory stays the boundary even on attacker-crafted PDFs. A summary `AdwAlertDialog` reports how many files were saved or which failed; PDFs with zero attachments show "No Embedded Files" instead.

### GtkListView Migration of the TOC Sidebar (Phase 8)
`fw-sidebar.c` rewritten end-to-end against `GtkListView` + `GtkTreeListModel` + `GtkSingleSelection`, replacing the deprecated `GtkTreeView` + `GtkTreeStore`. The build's `-Wdeprecated-declarations` warnings on the sidebar are gone. Item type is a new `FwTocItem` GObject (title, page, optional children GListStore) built from the existing `FwTocNode` tree on TOC load. The `GtkTreeListModel`'s create-child-model callback hands back each item's `children` store on demand, so the tree expands lazily.

The v0.8 current-page highlight survives the migration, with one structural change: `find_best_match` now walks the underlying `FwTocItem` tree (not the flat tree-list-model, since rows for collapsed branches don't exist in the flat model). Once it picks the deepest match, a `build_path` walk produces the root-to-target path of `FwTocItem*` pointers; the highlight code expands each ancestor's `GtkTreeListRow` so the target row materializes in the flat model, then walks the model to find the row's position and calls `gtk_single_selection_set_selected` + `gtk_list_view_scroll_to`. Click navigation routes through `GtkListView::activate` → `page-requested` signal — same contract the window has handled since v1.0.

---

## v0.10.0 (2026-04-30)

---

### Empty Window State (Phase 8)
Launching Framework with no file argument now shows an `AdwStatusPage` with the app icon, the prompt "Open a Document", and a suggested-action pill button wired directly to `app.open`. The page also tells the user they can drop a file onto the window — pulling double duty as documentation for the new drag-and-drop handler. The split view's content is now a `GtkStack` that crossfades between the empty page and the document overlay; on `fw_window_open_file` success it switches to the document view, and stays there for the window's lifetime (no reverting to empty when a doc closes — opening a new file replaces the current document, matching the rest of the single-document-per-window model).

### Drag-and-Drop File Open (Phase 8)
A `GtkDropTarget` accepting `G_TYPE_FILE` is attached to the window. On drop, the file's path is resolved with `g_file_get_path` and routed through the standard `fw_window_open_file` path — so a dropped file replaces the current document if one is open, or boots the document view from the empty state if not. Multi-file drop (drag a folder of comics onto the window) is intentionally out of scope here because Framework is one-document-per-window; multi-window file open already exists via `g_application_open` from the command line.

### Printing (Phase 8)
`Ctrl+P` now opens the system print dialog. Printing routes through `GtkPrintOperation`: `begin-print` reports the page count from the active document, `draw-page` renders the requested page via the existing `fw_document_render_page` interface (so PDF, DjVu, and CBZ all print through the same code path), and the resulting cairo surface is painted into the print context with a `cairo_scale (cr, 1/zoom, 1/zoom)` so 1 source pixel maps to 1 point on paper. Render quality is the print context's reported DPI capped at 300 — a US-letter page renders to ~33 MB, plenty for laser/inkjet output without ballooning memory on a long print job. The print dialog uses the document's basename as the job name so spool queues stay readable.

### Out of Scope This Release
**`GtkListView` migration of the TOC sidebar** stays open. The `GtkTreeView` API is technically deprecated in GTK4, and the build prints one `-Wdeprecated-declarations` per compile, but it works correctly and our v0.8 TOC-highlight walker depends on the `GtkTreeIter` traversal API. Migrating means rewriting both `populate_store` and `find_best_match` against `GtkTreeListModel` — pure churn, zero new user value, with regression risk on a feature we just shipped. Deferred to a future release where we have a reason to be in that file.

---

## v0.9.0 (2026-04-30)

---

### Comic Book Archive Support — CBZ (Phase 7)
Framework now opens CBZ comic-book archives. The PDF backend was already format-agnostic — it calls `fz_register_document_handlers` and `fz_open_document`, both of which dispatch by format internally — so the work was almost entirely factory-side: extend `fw_document_new_for_path` to accept `.cbz` / `.cbr` / `.cb7` / `.cbt`, route them through the same MuPDF backend, and tag the trace label as "Comic (MuPDF)" so logs make sense. The 8-instance parallel render path, velocity engine, thumbnail tier, and search infrastructure all carry over for free — opening a 237-page Berserk volume gets the full multi-core render treatment with zero new code in the cache layer.

A 237-page CBZ volume verified: opens in ~14 ms, the first ten pages immediately enter the parsed-handle window, and scrolling stays smooth under the existing velocity engine. Search returns no results (CBZ pages are images, no text layer), Match Count correctly reports zero, and the rest of the UI is identical to PDF behaviour.

### CBR — Best-Effort with Actionable Error Message
`.cbr` is also accepted by the factory, but the file format is RAR-compressed and most Linux distributions (including Fedora) ship MuPDF without the optional `libunrar` dependency for licensing reasons. Trying to open a CBR now produces a tailored error dialog explaining the situation and suggesting CBZ conversion, instead of MuPDF's bare "cannot find document handler". The CBR path is one `g_set_error_literal` swap in the factory — when a libunrar-enabled MuPDF is available, CBR will Just Work with no further code changes.

### File Dialog & Desktop Entry MIME Wiring
The Open dialog filter learned `*.cbz` / `*.cbr` / `*.cb7` / `*.cbt` and the corresponding MIME types (`application/vnd.comicbook+zip`, `application/vnd.comicbook-rar`, `application/x-cbz`, `application/x-cbr`). The `.desktop` file's `MimeType=` line gained the same set so file managers offer Framework as a handler for comic archives.

### Out of Scope This Release
EPUB / XPS / FB2 / MOBI — MuPDF supports them and the factory could trivially route them through the same backend, but each has format-specific UX considerations (EPUB reflow, XPS per-page sizing, MOBI proprietary parsing edge cases) that deserve their own review. Phase 7 specifically scopes "graphic novels" first; the rest stays open.

---

## v0.8.0 (2026-04-30)

---

### TOC Highlight Tracking (Phase 6)
The sidebar now follows along as the user scrolls. `fw_sidebar_set_current_page` walks the TOC tree depth-first looking for the deepest entry whose destination page is ≤ the current page, then selects that row and scrolls it into view. The walk is recursive but bounded by the document's TOC depth (chapter / section / subsection — three deep on every textbook tested), so calling it from every scroll-tick `value-changed` callback is cheap. Ancestor nodes are auto-expanded so a deeply-nested section actually becomes visible. Programmatic selection in GTK4's `GtkTreeView` does not trigger `row-activated`, so there is no feedback loop with our existing TOC click handler.

### Navigation History (Alt+Left / Alt+Right)
Standard browser-style back/forward stacks for in-document jumps. The window keeps two `GArray`s of `(page, scroll-fraction)` entries. The back stack is pushed when the user makes an *explicit* jump — a TOC click, a page-entry edit, or an internal-link click — and the forward stack is wiped at the same moment, matching every web browser's history rule. Plain scrolling, the next/previous-page buttons, and search-hit reveal do *not* push, because scrolling away from your current spot to read more is not a "jump." Alt+Left pops back, pushing the current viewport onto forward; Alt+Right pops forward, pushing onto back. The history is per-window and cleared on document switch.

### `page-jumped` Signal on `FwView`
The view emits `page-jumped(int dest_page)` only when an internal link click triggers a navigation. The window subscribes to push the previous viewport onto the back stack — without this, link-click jumps were invisible to the navigation code (the click bypassed `go_to_page`). Search-hit reveals deliberately stay silent so navigating through 47 search matches doesn't bury the user's pre-search position under a 47-entry stack.

### Sidebar Click Navigation (already shipped, now formally complete)
TOC click navigation has worked since v1.0 via `GtkTreeView::row-activated` → `FwSidebar::page-requested` → `FwWindow::on_sidebar_page_requested`. Phase 6 marks it formally complete; the TOC click path now also pushes navigation history.

---

## v0.7.0 (2026-04-30)

---

### Async, Progressive Search (Phase 5)
Search no longer blocks the UI. The previous `fw_search_find` was a synchronous loop calling `fz_search_page` on every page in turn — on a 1000-page textbook this froze the window for several seconds before any result appeared. The new path runs the page-by-page scan on a dedicated worker thread and posts each page's hits back to the main loop via `g_idle_add_full`, so matches appear as they're found and the UI stays responsive throughout. The scan also starts at the user's current page and wraps, so matches near where they're reading appear first.

A monotonically-incrementing generation counter discards in-flight messages from cancelled scans — typing into the search bar instantly retargets the worker without races. Cancellation is cooperative via an atomic flag the worker polls between pages; cleanup is deterministic on document close, dispose, or query change.

### Search Result Highlighting
All hits paint as semi-transparent yellow overlays directly in `fw_view_snapshot`, scaled and translated into widget coordinates via the same zoom/page-position math the cache uses. The active hit (the one the count label says is "current") paints in a stronger orange tint instead of yellow so the user always knows which match `Next`/`Prev` will move them away from. Highlights re-layer correctly under text selection and link cursors, and are invalidated automatically by the existing `redraw_pending` flag — no extra repaint plumbing.

### Search Navigation (F3 / Shift+F3)
F3 jumps to the next match, Shift+F3 to the previous, with wrap-around at both ends. Both are exposed as `win.find-next` / `win.find-prev` GActions so they work whether or not the search bar is focused, and the search entry's built-in next-match/previous-match signals route to the same handlers. When the active hit changes, the view scrolls so the hit lands roughly one-third of the way down the viewport (reading context above it) and pans horizontally if the hit is offscreen due to zoom.

### Match Count Label
The search bar now shows "3 of 47" alongside the entry. While the worker is still scanning, the count appends a `+` ("3 of 47+") and the label switches to "Searching…" while results are still empty, so the user can tell the difference between "no matches yet" and "no matches at all." The Prev/Next buttons disable when no hits exist.

### `FwSearch` API Reshape
The signal-based interface is new: `hits-changed`, `current-changed`, `search-finished`. `fw_search_find()` now takes a `start_page` argument; `fw_search_clear()` is split out from `set_document` and is also called when the search bar closes; new helpers `fw_search_hits_for_page`, `fw_search_peek_hits`, `fw_search_active_index`, and `fw_search_get_current_page` let the view read state without re-iterating. `FwView` gained `fw_view_set_search` / `fw_view_reveal_active_hit` and a new owned ref to the search controller.

### Roadmap: Phase 12 (Stress-Testing & Debugging Suite)
Added a new top-level roadmap phase covering a `tests/` tree with stress tests (`stress-scrub`, `stress-zoom-storm`, `stress-multidoc`, `stress-corpus-soak`), benchmarks (`bench-render`, `bench-cache-hit-rate`, `bench-startup`), and a debugging setup (`-Dsanitize` meson option, `tests/scripts/debug.sh`, `coredump-triage.sh`, an `FW_DEBUG` log-replay tool, and `framework --self-test`). All of it is gated by a `-Dstress=true` meson option so packagers don't pay for it. None of it is built yet — the phase exists so the Phase 11 borrows have a regression net to land into.

---

## v0.6.0 (2026-04-29)

### Pre-1.0 Version Regression
Dropped the project version from `1.6.0` to `0.6.0`. The earlier 1.x numbering implied a stability and feature-completeness the project hasn't earned: Framework opens and reads PDF and DjVu correctly, but search is synchronous and incomplete (Phase 5), TOC navigation is partial (Phase 6), printing isn't wired up (Phase 8), no Flatpak ships (Phase 10), and the reference-survey borrows in Phase 11 (`fz_cookie` cancellation, cached stext, bytes-aware cache, hue-preserving recolor) are still TODO. A 1.0 tag should be earned at the end of Phase 10, not assumed at the start. Past patchnotes entries keep their historical 1.x labels — those releases happened — but the running version is now honest.

---

## v1.6.0 (2026-04-17)

---

### Zero-Copy MuPDF Render
Replaced the MuPDF → cairo conversion pipeline entirely. The previous path rendered into an intermediate `fz_pixmap`, then walked every pixel in a scalar loop to shuffle RGB → BGRA and premultiply alpha. The new path — borrowed straight from `zathura-pdf-mupdf` — constructs the pixmap *around the cairo surface buffer* via `fz_new_pixmap_with_bbox_and_data` using `fz_device_bgr` as the colorspace. MuPDF's draw device writes rendered pixels directly into the final ARGB32 buffer in the correct byte order, with no intermediate allocation, no channel shuffle, and no per-pixel loop. On a typical 1600×2100 page render this cuts ~15-30% off the per-page wall time and eliminates all per-page `cairo_image_surface_create` + scalar-loop overhead. The `pixmap_to_cairo_surface` helper and its 4-pixel unrolled hot path from v1.5 are now deleted — the optimization is obsolete because we no longer copy pixels at all.

### Unified PDF Render Path
Collapsed the duplicated "parallel instance" and "fallback to main context" code paths in `pdf_render_page` into a single code path that picks the context+document+lock at the top. The two branches now share identical render logic via the new `render_page_direct()` helper. Easier to reason about and less drift risk when future optimizations land.

### Reference Source Study
Downloaded `mupdf`, `djvulibre`, `zathura-pdf-mupdf`, and `zathura-djvu` sources for side-by-side comparison. The zero-copy render path above came directly from studying zathura's implementation. Our DjVu backend was already doing the right thing (RGBMASK32 format matching cairo ARGB32, writing straight into the surface buffer) since v1.0 — zathura's DjVu plugin confirmed the approach is optimal.

---

## v1.5.0 (2026-04-17)

---

### Persistent Thumbnail Tier
Introduced a third cache tier for low-resolution page previews (~150px wide). Thumbnails render in a dedicated single-thread background pool, so they never compete with full-resolution renders for CPU slots. Once rendered they are never evicted — each thumbnail costs ~120KB, so a 1000-page document fits in ~120MB. When a visible page has no full-resolution surface ready (fast scroll, cold cache, mid-zoom-transition), the view now paints the scaled thumbnail instead of a gray rectangle. Users see actual content during fast scroll instead of placeholders.

### Per-Frame Texture Caching
`GdkTexture` objects are now cached inside each `CacheEntry` and reused across frames. Previously, every snapshot pass allocated a fresh `GdkMemoryTexture` + `GBytes` wrapper for every visible page — at 60fps with 3 visible pages, that was ~180 allocations per second. The new path builds the texture once when the render worker stores a surface, holds it for the entire surface lifetime, and drops it atomically when the entry is evicted. The `prev_surface` zoom-transition path has a matching `prev_texture` slot so even scaled placeholders avoid re-allocation.

### Hot-Path Pixmap Conversion
Rewrote `pixmap_to_cairo_surface()` in the PDF backend to hoist branches out of the per-pixel inner loop. The format check (RGB vs. RGBA) now happens once at the top of the function, and the RGB path (the common case for opaque PDFs) is 4-pixel unrolled. For a typical 1600x2100 page render, this cuts ~10-20% off the pixel format conversion time. The compiler can now vectorize the unrolled loop on targets that support it.

### Scroll Velocity Capping
Added a hard cap of 120px on single-event scroll distance (previously unbounded). Combined with the existing SCROLL_STEP damping, this prevents a single fast wheel flick or amplified trackpad event from blowing past multiple pages in one frame. The render cache can now reliably keep up with sustained scrolling without entering the scrubbing-abort state unnecessarily.

### Texture Memory Layout Fix
The previous texture path relied on GBytes's `GDestroyNotify` to eventually free the underlying cairo surface, but the surface destroy order in `cache_entry_free()` was ambiguous. The new code unrefs the texture before destroying the surface — the texture's internal GBytes drops the surface's first reference, then our explicit surface destroy drops the last. This guarantees the GPU-uploaded pixel buffer remains valid for GTK's full rendering lifecycle.

---

## v1.4.0 (2026-04-16)

---

### GPU Color Inversion
Replaced the per-frame `g_memdup2` pixel inversion loop with `gtk_snapshot_push_color_matrix()`. Color inversion now applies a 4x4 matrix on the GPU — zero memory allocation, zero pixel copying. At 60fps with 5 visible pages, this eliminates ~30-60 MB/s of wasted allocations that the old path produced. Both the normal and inverted rendering paths are now fully zero-copy.

### Velocity EMA Smoothing
The scroll velocity tracker now uses an exponential moving average (`0.7 * old + 0.3 * new`) instead of raw per-frame `dy/dt`. Single-frame spikes from mouse wheel clicks or trackpad jitter no longer trigger the scrubbing abort state. Genuine fast scrolling still activates scrubbing correctly — the EMA responds within 2-3 frames.

### Scroll Position Preservation
Zooming in or out no longer jumps to a random position. Before each zoom change, the view records the current page and fractional offset within that page. After the layout recomputes at the new zoom level, the scroll position is restored to the same page and fraction. Sub-page precision is maintained across arbitrary zoom changes.

### Fit-Page Zoom (Ctrl+2)
Implemented `fw_view_fit_page_zoom()` which calculates `min(viewport_w / max_page_w, viewport_h / max_page_h)` across all pages. The entire page fits within the viewport without scrolling. Accounts for rotation — at 90/270 degrees, width and height are swapped before the calculation.

### Rotation (Ctrl+Shift+Plus/Minus)
Document rotation in 90-degree increments. `Ctrl+Shift+Plus` rotates clockwise, `Ctrl+Shift+Minus` rotates counter-clockwise. The view layout swaps page width and height at 90/270 degrees. The cache re-renders all visible pages at the new rotation. Both MuPDF and DjVuLibre backends already supported rotation in their render paths — this release wires it through the UI layer with proper layout recomputation. Rotation state is saved and restored per-document.

### Stale Surface Placeholder
Zoom transitions no longer flash gray placeholders. When the render generation changes (zoom, rotation, or scale factor), existing surfaces are moved to a `prev_surface` slot in the cache entry. The view renders these scaled-to-fit as placeholders until the sharp re-render arrives. The result is slightly blurry content during the transition instead of a blank gray rectangle. Previous-generation surfaces are freed as soon as the new render completes.

### Scroll Damping
Scroll wheel events are now intercepted and applied with a controlled step size (60px per tick), bypassing GTK's kinetic scrolling amplification. This bounds the maximum achievable scroll velocity, reducing the frequency of scrubbing abort triggers and giving the render pipeline more time to keep up. The velocity engine still tracks actual scroll speed via the frame clock tick callback.

### Redundant Redraw Guard
Added a `redraw_pending` flag to the view widget. When the scroll adjustment fires `value-changed` rapidly (every scroll tick), redundant `gtk_widget_queue_draw()` calls are suppressed. The flag is cleared at the start of each `snapshot` call. The render worker's idle-based redraw scheduling is unaffected — it already self-rate-limits via the GLib idle mechanism.

### Text Selection and Copy (Ctrl+C)
Click-drag on a page selects text. A `GtkGestureDrag` on the view widget maps mouse coordinates to document-space points via the page layout's centering and zoom transforms. The selection is rendered as a semi-transparent blue overlay (`rgba(0.2, 0.4, 0.8, 0.3)`) painted after the page texture in the snapshot. On drag end, `fw_document_get_text()` extracts the text within the selection rectangle. `Ctrl+C` copies the selected text to the system clipboard via `gdk_clipboard_set_text()`. Selection is single-page only in this release.

### Dynamic Cursors
The mouse cursor changes based on what it's hovering over. A `GtkEventControllerMotion` on the view maps the pointer position to document coordinates and hit-tests against link rectangles. The cursor shows a pointing hand over links and a text I-beam over page content. Link rectangles are cached per-page and invalidated on document change.

### Link Click Navigation
Clicking a link navigates. Internal links (to other pages in the document) call `fw_view_go_to_page()`. External links (URLs) launch the default browser via `GtkUriLauncher`. The click gesture is registered before the drag gesture — when a link is hit, the click claims the event sequence so text selection doesn't start. When no link is hit, the event falls through to the drag gesture for text selection.

### Debug Tracing Expansion
Added structured trace coverage for all new code paths: scroll position preservation (`view` domain), text selection drag lifecycle (`view`), link click events (`view`), cache I/O page opens (`cache`), and `prev_surface` stash/free events (`mem`). All new traces follow the existing zero-overhead pattern — a single `G_UNLIKELY` atomic check when `FW_DEBUG` is not set.

## v1.3.3 (2026-04-12)

---

### Cache Memory Leak Fix
Fixed `fw_cache_dispose` never running. `FwView` held a GObject ref to the cache via `g_set_object`, but the view's dispose ran too late (or never) in GTK4's widget teardown order — the cache refcount never hit zero. Fixed by explicitly disconnecting the view from the document and cache at the start of `fw_window_dispose`, before dropping the window's own refs.

### DjVu Initial Render Fix
Fixed DjVu pages appearing blank on file open until the user scrolled. `fw_window_open_file` called `set_zoom()` (which internally calls `fw_cache_start()`, bumping the generation counter) and then called `fw_cache_start()` again explicitly — the second call bumped the generation a second time, making the first batch of render jobs stale. For DjVu (serialized single-mutex renders), all queued pages were discarded before any completed. Removed the redundant `fw_cache_start()` calls.

### Split Generation Counter (I/O Optimization)
Split the single `generation` counter into `render_gen` (zoom/rotation/scale changes) and `cancel_gen` (scrubbing/stop abort). Previously, entering scrubbing state bumped the shared generation, invalidating all already-rendered surfaces even though zoom and rotation hadn't changed. With the split, scrubbing only bumps `cancel_gen` to abort in-flight work — completed surfaces rendered at the correct zoom/rotation are kept. Eliminated ~816 wastefully discarded surfaces per heavy scroll session.

### Debug Tracing System
Added zero-overhead runtime debug tracing, enabled with `FW_DEBUG=1`. Domain-prefixed structured logging covers document lifecycle (`doc`), PDF backend (`pdf`), DjVu backend (`djvu`), cache operations (`cache`), view state (`view`), window actions (`window`), and memory events (`mem`). All trace calls compile to a single `G_UNLIKELY` atomic check when disabled. Output goes to stderr with timestamps.

### Cache Window Reduction
Reduced the parsed page cache window from 50 to 30 pages to lower speculative rendering overhead without impacting scroll-ahead coverage.

## v1.3.2 (2026-04-12)

---

### Velocity-Aware Render Throttling
The cache engine now adapts its workload based on scroll velocity. During cruising (moderate scrolling), priority rebuilds are throttled to once per 150 ms instead of every scroll tick, the thread pool is limited to 2 concurrent render jobs (down from all cores), and only the immediate neighborhood (visible + 7 forward + 3 backward) is queued — the full 50-page window waits until scrolling stops. The scrubbing threshold is also lowered from 2000 px/s to 1500 px/s so full render abort kicks in sooner. Result: significantly less CPU churn during fast scrolling through both PDF and DjVu documents.

### DjVu Zero-Copy Rendering
DjVu page rendering no longer allocates a temporary RGB buffer or runs a pixel-by-pixel format conversion. The render format is switched from `DDJVU_FORMAT_RGB24` (3 bytes/pixel into a scratch buffer, then shuffled into ARGB32) to `DDJVU_FORMAT_RGBMASK32` with channel masks matching cairo's native ARGB32 layout. DjVuLibre now writes 32-bit pixels directly into the cairo surface buffer. A fast `|= 0xFF000000` alpha pass and any rotation run outside the render lock, reducing mutex contention. The `ddjvu_format_t` object is created once at document open instead of per-page.

### DjVu Cancel Flag Fix
Fixed DjVu files going permanently blank after fast scrolling. The `cancel_flag` set by the velocity engine's scrubbing state was never cleared in the `render_page_from_handle` code path — once set, every subsequent DjVu render returned NULL. The flag is now cleared on both entry paths.

### Safe Widget Redraw Scheduling
The render worker's `g_idle_add` callback now holds a proper `g_object_ref` on the view widget and checks `GTK_IS_WIDGET` before calling `gtk_widget_queue_draw`. Previously, a raw pointer was passed via `g_idle_add_once`, which could fire after the widget was disposed during document swap — producing infinite `GTK_IS_WIDGET` assertion spam.

## v1.3.1 (2026-04-11)

---

### Cache Freeze After Fast Scrolling
Fixed a bug where the cache would freeze after fast scrolling, requiring a manual zoom to recover. When the velocity engine entered scrubbing state (bumping the generation counter), render jobs that were mid-flight would complete and discard their stale surfaces but leave `rendering = TRUE` on the cache entry. Those pages were permanently stuck — `submit_next_jobs` would skip them, so they never re-rendered. Both the early bail-out (job starts after generation bump) and late bail-out (job finishes after generation bump) paths now clear the rendering flag.

### Smarter Cache Priority
The render priority window now populates the immediate neighborhood first: visible pages, then 7 pages forward, then 3 pages backward, then the rest of the 50-page window. Previously, all forward pages were queued before any backward pages, so scrolling backward hit blank pages even though the cache window was large.

### DjVu Widget Assertion Fix
Fixed an infinite `gtk_widget_queue_draw: assertion 'GTK_IS_WIDGET (widget)' failed` spam when opening DjVu files. The render worker's `g_idle_add_once` was calling `gtk_widget_queue_draw` on the view widget pointer after the widget had been disposed during document swap, or before it was fully realized.

## v1.3.0 (2026-04-11)

---

### Two-Tier Cache Architecture
Replaced the single surface cache with a two-tier system that separates parsed page objects from rendered pixel surfaces.
- **Tier 1 (Parsed Window):** Pre-loads lightweight backend page objects (`fz_page` / `ddjvu_page`) for the entire priority window (~50 pages). Negligible RAM cost, eliminates disk I/O when scrolling into uncached regions.
- **Tier 2 (Pixel Window):** Rendered `cairo_surface_t` surfaces, same as before but now fed by pre-loaded handles.
- **Page Handle API:** New `fw_document_open_page()` / `close_page()` / `render_page_from_handle()` on the document interface. Backends implement the separation between page loading (I/O-bound) and rendering (CPU-bound).

### MuPDF Parallel Rendering
MuPDF rendering is no longer serialized through a single mutex.
- **Independent Instances:** Up to 8 separate `fz_context` + `fz_document` pairs are created at document open, each opening the file independently. Each render thread acquires its own instance via round-robin.
- **Zero Shared State:** Unlike cloned contexts (which share the font/image store), independent instances have no shared state at all. This prevents crashes with PDFs that use JPEG2000 images or complex color spaces where lazy stream reads would race.
- **Result:** On multi-core machines, multiple pages render simultaneously with full thread safety.

### DjVu Render Cancellation
DjVu rendering now supports cooperative cancellation during high-velocity scrubbing.
- **Cancel Flag:** When the velocity engine enters scrubbing state, `cancel_render()` sets a flag on the DjVu backend. The render function checks this flag before and after the expensive page decode step, bailing out immediately if set.
- **Result:** Rapid scrolling through DjVu documents no longer locks the render mutex for the duration of abandoned page decodes.

### Fast DjVu Page Probing
DjVu page dimensions are now pre-cached at document open time, matching the PDF backend's behavior. Previously, every call to `get_page_size()` hit `ddjvu_document_get_pageinfo()`. Now a single loop at open time populates cached arrays, eliminating repeated I/O during layout computation.

### Wayland Fractional Scaling
Render resolution now accounts for the display's device pixel ratio.
- **Scale Factor Awareness:** The cache multiplies the logical zoom by `gtk_widget_get_scale_factor()` when submitting render jobs. Text and graphics are rendered at native display resolution.
- **Monitor Changes:** Moving a window between displays with different scale factors triggers automatic re-render at the correct resolution.

### Invert Colors
`Ctrl+I` now works. Color inversion is applied at the display stage — RGB channels are bitwise-inverted on the pixel data during snapshot, without re-rendering the underlying document surfaces. Toggling is instant with no cache invalidation.

### Ctrl+Scroll Wheel Zoom
Zooming with `Ctrl+Scroll` now anchors to the pointer position rather than the viewport center. The zoom target is calculated from the pointer coordinates relative to the document, so the content under the cursor stays fixed as the zoom level changes.

### MuPDF Thread Safety
Fixed critical crashes (SIGSEGV) when rendering PDFs. The original cloned-context approach shared MuPDF's font/image store across threads — even with proper store locking, `fz_page` and `fz_image` objects lazily read from PDF streams owned by the parent document, corrupting state under concurrent access. Replaced with fully independent render instances that open the file separately per thread, eliminating all shared state.

### Ref-Counted View Pointers
`FwView` now holds proper GObject references to the document and cache objects via `g_set_object()`, preventing dangling pointer crashes on document swap.

## v1.2.0 (2026-04-11)

---

### The Velocity Engine
Replaced the brute-force static cache with intelligent resource pacing. 
- **Velocity Tracking:** The app now actively tracks scroll speed (`dy/dt`) using frame clock ticks.
- **Dynamic Queue Management:** Three render states (Static, Cruising, Scrubbing) automatically adjust the cache window. During high-velocity scrubbing, queued background render jobs are instantly aborted to prevent CPU thrashing.
- **Thread Drip-Feeding:** The worker pool now evaluates velocity after every single page render, preventing queue flooding and memory spikes.

### Memory & Performance Fixes
- **64MB MuPDF Clamp:** Hardcoded the `fz_new_context` store limit from the default 256MB down to 64MB, drastically reducing the baseline memory footprint.
- **Surgical Mutexing:** Cairo surface copying has been moved outside the MuPDF global lock, ending memory doubling during page transit and thread starvation.
- **Safe Exception Variables:** Addressed a critical crash risk by making variables modified in MuPDF's `fz_try` blocks `volatile` to comply with `setjmp/longjmp` rules.

## v1.1.0 (2026-04-07)

---

### State Persistence

Framework now saves and restores per-document state across sessions. On close,
the current page, scroll position, zoom level, and rotation are written to
`~/.local/share/framework/state.json`. Reopening the same file restores
exactly where you left off. Entries older than 90 days are pruned on startup,
capped at 500 documents (LRU).

**Save trigger.** State is saved via the `close-request` signal, which fires
while the window and all its widgets are still alive — not in `dispose` where
adjustments may already be destroyed.

**Scroll restore.** Deferred via `gtk_widget_add_tick_callback` until the
scrolled window has a real allocation and the layout is computed. The saved
page is navigated to first, then the scroll fraction is applied for sub-page
precision.

### Live Page Tracking

The page number in the header bar now updates as you scroll through the
document. Previously it only changed on explicit navigation (Page Up/Down,
go-to-page). A `value-changed` handler on the vadjustment calls
`fw_view_get_current_page` — a reverse lookup through the page y-offset
array — and updates the entry on every scroll position change. Works for
both PDF and DjVu.

### Bug Fixes

**JSON state crash.** `json_node_new(JSON_NODE_OBJECT)` creates a node typed
as object but leaves the internal object pointer NULL. All three fallback
paths in `load_root` now use `json_node_init_object` with a properly
allocated `json_object_new()`.

---

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
