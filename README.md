<p align="center">
  <img src="logo.svg" alt="Framework" width="420">
</p>

<p align="center">
  <a href="https://www.gtk.org/"><img src="https://img.shields.io/badge/GTK4-libadwaita-4a86cf" alt="GTK4"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--3.0-blue.svg" alt="License: GPL-3.0"></a>
  <a href="https://ko-fi.com/vrnvctss"><img src="https://img.shields.io/badge/support-Ko--fi-ff5f5f?logo=kofi" alt="Ko-fi"></a>
</p>

---

# Framework

A fast, native GNOME document viewer built on MuPDF and DjVuLibre. Framework is engineered for performance, utilizing aggressive pre-caching and a modern libadwaita UI to provide a "SumatraPDF-like" experience for Linux.

## Why this exists

Linux document viewers often fall into two categories: feature-heavy clients (like Okular) that bring extensive dependencies to GNOME, or minimal MuPDF wrappers that lack a functional UI. Framework fills the gap by providing a native, high-performance GNOME solution that prioritizes rendering speed and kinetic scrolling without the bloat of an editor.

## Features

| Feature | Description |
|---------|-------------|
| **Velocity Engine** | Dynamic cache management that throttles render jobs based on scroll speed. |
| **Two-Tier Cache** | Separates parsed page objects from rendered surfaces to minimize I/O. |
| **Parallel Rendering** | Independent MuPDF instances render pages across multiple CPU cores. |
| **Zero-Copy DjVu** | Full DjVuLibre support with zero-copy rendering into Cairo surfaces. |
| **HiDPI Scaling** | Native device pixel ratio rendering for sharp text on Wayland. |

## Development & Build

### Requirements
- `gtk4` (4.16+), `libadwaita` (1.7+)
- `mupdf` (1.24+), `djvulibre` (3.5.28+)
- `meson` (1.4+)

### Build Pipeline
```bash
meson setup builddir
meson compile -C builddir
```

## What Framework is not

Framework is strictly a **viewer**. It is not an editor (no annotations), not a library manager, and not an image viewer. It focuses on doing one thing exceptionally well: opening and displaying documents.

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
| Find | Ctrl+F |
| Invert colors | Ctrl+I |

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

## Support

If this saved you time, consider [buying me a coffee](https://ko-fi.com/vrnvctss).
