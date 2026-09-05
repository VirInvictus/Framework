<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Test corpus and stress suite

The stress and benchmark targets under `tests/stress/` run against real
documents. Those documents are copyrighted, so they are never committed;
they live in a gitignored `.testfiles/` directory at the repo root that
you populate yourself.

## Populating `.testfiles/`

Copy one file per format into `.testfiles/`, using these exact names
(the suite and `corpus.json` resolve them by name):

| Filename | Format | What it exercises |
|---|---|---|
| `effective-java.pdf` | PDF | Textbook baseline (~900 pages, font/code-heavy). Used by the scrub, zoom, and search-cache tests. |
| `visual-explanations-tufte.pdf` | PDF | Image-heavy PDF. |
| `on-growth-and-form.djvu` | DjVu | Scanned book (single-mutex DjVu path). |
| `playing-at-the-world-v2.epub` | EPUB | Reflow + search; image and footnote heavy. |
| `the-broken-god.mobi` | MOBI / KF7 | Reflow parser, filepos TOC. |
| `datapoint.azw3` | AZW3 / KF8 | Reflow parser, KF8 INDX/SKEL path. |
| `nausicaa-v01.cbz` | CBZ | ZIP comic via MuPDF. |
| `vagabond-v01.cbr` | CBR | RAR comic via libarchive. |

Any name can stand in for a different document of the same format; the
filenames just have to match. Missing files are skipped, not failed, so
a partial corpus still runs (you'll see `skip (missing)` lines).

To use a corpus elsewhere on disk (for example your full library, for a
heavier memory-pressure soak), set `FW_TEST_CORPUS_ROOT` to a directory
containing files with these names. It overrides the `.testfiles/`
default at meson configure time.

## Building and running

The suite is gated behind `-Dstress=true` (off by default):

```sh
meson setup builddir -Dstress=true
meson compile -C builddir
meson test -C builddir
```

Registered tests: `stress-scrub`, `stress-zoom-storm`,
`stress-search-cache`, `stress-multidoc`, `stress-corpus-soak`,
`stress-reflow`. The benchmarks (`bench-render`, `bench-startup`,
`bench-cache-hit-rate`) are built but not registered; invoke them
directly.

`stress-reflow` covers the reflow pipeline (the EPUB/MOBI/AZW3/FB2/TXT/MD
backends and the search core) at the document layer: parsing,
`produce_html` output, and content scrubbing. Presentation — pagination,
highlight rendering, themes, typography — happens inside the
`WebKitWebView` (`fw-webview.c`), so it is verified by running the app
rather than in the headless suite.

## Sanitizers

```sh
meson configure builddir -Dsanitize=address,undefined
meson compile -C builddir
meson test -C builddir
meson configure builddir -Dsanitize=    # revert
```

Note: `stress-corpus-soak`'s RSS cap has little headroom under ASan. If
it is the only failure and reports `failures=0`, that is sanitizer
memory overhead, not a regression; confirm on a non-sanitized build.
