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

djvu_dep       = dependency('ddjvuapi',    version: '>= 3.5.28')
archive_dep    = dependency('libarchive', version: '>= 3.6')
fontconfig_dep = dependency('fontconfig')
libxml_dep     = dependency('libxml-2.0',  version: '>= 2.9')
# Reflow rendering tier: WebKitGTK 6.0 renders EPUB (and incrementally the
# other reflow formats) via the in-runtime web engine. Foliate uses the
# same library for the same reason: EPUB is HTML+CSS underneath, and
# reusing WebKit's layout/typography is a massive win over the prior
# block-model pipeline. Shipped in the GNOME 50 Platform runtime.
webkit_dep     = dependency('webkitgtk-6.0', version: '>= 2.46')
m_dep          = cc.find_library('m', required: false)

# Source list, partitioned so tests can link against the engine without
# pulling in main(). Everything except main.c is compiled into a static
# library; the framework executable links it together with main.c.
framework_lib_sources = files(
  'fw-application.c',
  'fw-window.c',
  'fw-view.c',
  'fw-cache.c',
  'fw-document.c',
  'fw-document-pdf.c',
  'fw-document-djvu.c',
  'fw-document-cbr.c',
  'fw-reflow-document.c',
  'fw-reflow-html.c',
  'fw-reflow-document-txt.c',
  'fw-reflow-document-fb2.c',
  'fw-reflow-document-epub.c',
  'fw-reflow-document-mobi.c',
  'fw-mobi-parser.c',
  'fw-reflow-view.c',
  'fw-reflow-sidebar.c',
  'fw-sidebar.c',
  'fw-search.c',
  'fw-state.c',
  'fw-debug.c',
  'fw-sandbox.c',
  'fw-fonts.c',
  'fw-webview.c',
)

framework_main_sources = files('main.c')

framework_deps = [
  gtk_dep,
  adw_dep,
  cairo_dep,
  glib_dep,
  json_glib_dep,
  mupdf_dep,
  djvu_dep,
  archive_dep,
  fontconfig_dep,
  libxml_dep,
  webkit_dep,
  m_dep,
]

# Sanitizer flags from the user-facing -Dsanitize= option. Forwarded to
# every target so the executable and tests stay compatible.
sanitize_flags = []
foreach s : get_option('sanitize')
  sanitize_flags += '-fsanitize=' + s
endforeach
if sanitize_flags.length() > 0
  add_project_arguments(sanitize_flags, language: 'c')
  add_project_link_arguments(sanitize_flags, language: 'c')
endif

framework_lib = static_library(
  'framework-core',
  framework_lib_sources,
  dependencies: framework_deps,
  install: false,
)

framework_lib_dep = declare_dependency(
  link_with: framework_lib,
  include_directories: include_directories('.'),
  dependencies: framework_deps,
)

executable('framework',
  framework_main_sources,
  dependencies: framework_lib_dep,
  install: true,
)
