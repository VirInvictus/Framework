desktop_file = i18n.merge_file(
  input: 'com.github.vrnvctss.framework.desktop.in',
  output: 'com.github.vrnvctss.framework.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: get_option('datadir') / 'applications',
)

appstream_file = i18n.merge_file(
  input: 'com.github.vrnvctss.framework.metainfo.xml.in',
  output: 'com.github.vrnvctss.framework.metainfo.xml',
  po_dir: '../po',
  install: true,
  install_dir: get_option('datadir') / 'metainfo',
)

install_data(
  'com.github.vrnvctss.framework.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

gnome.compile_schemas(build_by_default: true)

subdir('icons')
