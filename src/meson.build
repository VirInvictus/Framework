# SPDX-License-Identifier: GPL-3.0-or-later

gtk_dep = dependency('gtk4', version: '>= 4.16')
adw_dep = dependency('libadwaita-1', version: '>= 1.7')
cairo_dep = dependency('cairo', version: '>= 1.18')
glib_dep = dependency('glib-2.0', version: '>= 2.82')
json_glib_dep = dependency('json-glib-1.0', version: '>= 1.10')

# MuPDF's pkg-config is often broken (empty -I flag), so always use manual detection
cc = meson.get_compiler('c')
mupdf_dep = declare_dependency(
  dependencies: [
    cc.find_library('mupdf'),
    cc.find_library('mupdf-third', required: false),
  ],
)

djvu_dep = dependency('ddjvuapi', version: '>= 3.5.28')

framework_sources = files(
  'main.c',
  'fw-application.c',
  'fw-window.c',
  'fw-view.c',
  'fw-cache.c',
  'fw-document.c',
  'fw-document-pdf.c',
  'fw-document-djvu.c',
  'fw-sidebar.c',
  'fw-search.c',
  'fw-state.c',
  'fw-debug.c',
)

framework_deps = [
  gtk_dep,
  adw_dep,
  cairo_dep,
  glib_dep,
  json_glib_dep,
  mupdf_dep,
  djvu_dep,
]

executable('framework',
  framework_sources,
  dependencies: framework_deps,
  install: true,
)
