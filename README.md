<p align="center">
  <img src="logo.svg" alt="Framework" width="420">
</p>
<p align="center">
  <a href="https://www.gtk.org/"><img src="https://img.shields.io/badge/GTK4-libadwaita-4a86cf" alt="GTK4"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--3.0-blue.svg" alt="License: GPL-3.0"></a>
  <a href="https://ko-fi.com/vrnvctss"><img src="https://img.shields.io/badge/support-Ko--fi-ff5f5f?logo=kofi" alt="Ko-fi"></a>
</p>

A fast, native GNOME document viewer built on MuPDF and DjVuLibre. Opens PDFs and DjVu files with aggressive pre-caching, a clean libadwaita UI, and zero bloat. It is a viewer — not an editor, not a library manager, not a file organizer.

## Why this exists

I was essentially looking for a SumatraPDF-like client for Linux. I was frustrated with clients that used libPoppler and every muPDF client lacked any UI whatsoever and relied on keyboard-shortcuts. I wanted something in-between, as I was using Okular in a Gnome environment because it was the only one that took everything in stride. I wanted a Gnome solution. This is that.

## Features

| Feature | Description |
|---------|-------------|
| **MuPDF backend** | Fast PDF rendering via libmupdf with per-thread context safety |
| **DjVuLibre backend** | Full DjVu/DjV support via ddjvuapi |
| **Pre-cache engine** | Thread pool renders pages asynchronously with priority ordering — visible pages first, then forward, then backward |
| **Fit-width default** | Pages scale to viewport width on open, regardless of source dimensions |
| **Continuous scroll** | All pages stacked vertically with smooth kinetic scrolling |
| **TOC sidebar** | Expandable tree view populated from document outline, toggle with F9 |
| **Search** | Full-text search across all pages (Ctrl+F) |
| **Keyboard-first** | Full shortcut parity with SumatraPDF (see below) |
| **State persistence** | Saves page, scroll position, and zoom per document — reopens where you left off |
| **Velocity Engine** | Dynamic cache management tracks scroll speed to pace rendering, abort stale jobs, and minimize memory footprint |

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
| Zoom in | Ctrl+Plus, Ctrl+= |
| Zoom out | Ctrl+Minus |
| Fit width | Ctrl+1 |
| Fit page | Ctrl+2 |
| Actual size (100%) | Ctrl+0 |

### View

| Action | Shortcut |
|--------|----------|
| Toggle sidebar | F9 |
| Fullscreen | F11 |
| Find | Ctrl+F |

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
- **Not an ebook reader.** No EPUB, no MOBI, no reflow

## Support

If this saved you time, consider [buying me a coffee](https://ko-fi.com/vrnvctss).
