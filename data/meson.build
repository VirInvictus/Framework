# SPDX-License-Identifier: GPL-3.0-or-later

desktop_file = i18n.merge_file(
  input: 'io.github.virinvictus.framework.desktop.in',
  output: 'io.github.virinvictus.framework.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: get_option('datadir') / 'applications',
)

appstream_file = i18n.merge_file(
  input: 'io.github.virinvictus.framework.metainfo.xml.in',
  output: 'io.github.virinvictus.framework.metainfo.xml',
  po_dir: '../po',
  install: true,
  install_dir: get_option('datadir') / 'metainfo',
)

install_data(
  'io.github.virinvictus.framework.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

gnome.compile_schemas(build_by_default: true)

subdir('icons')
