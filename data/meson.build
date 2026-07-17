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

# Owned application stylesheet (v0.80.0 de-adwaita). Loaded at startup by
# fw-theme.c from ${datadir}/framework/style.css (dev runs fall back to
# the source tree). This is the single styling authority now that
# libadwaita is gone.
install_data(
  'style.css',
  install_dir: get_option('datadir') / 'framework',
)

gnome.compile_schemas(build_by_default: true)

# Bundled reading fonts (Phase 13.1). Three OFL families:
#   * OpenDyslexic — accessibility / dyslexic-friendly
#   * Atkinson Hyperlegible — high-readability sans (default)
#   * Crimson Pro — body serif option
# Installed to ${datadir}/framework/fonts/, registered with FontConfig
# at app startup by fw-fonts.c. Each subdir ships an OFL.txt license.
foreach family : ['OpenDyslexic', 'AtkinsonHyperlegible', 'CrimsonPro']
  install_subdir('fonts' / family,
    install_dir: get_option('datadir') / 'framework' / 'fonts',
    exclude_files: [],
  )
endforeach

subdir('icons')
