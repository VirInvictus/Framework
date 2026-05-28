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

# Hyphenation patterns (Phase 16 Pillar 1). en_US TeX patterns from
# hyph-utf8, vendored verbatim with original license preserved.
# Loaded at runtime by src/fw-hyphenate.c to inject soft hyphens.
install_subdir('hyphenation',
  install_dir: get_option('datadir') / 'framework',
)

subdir('icons')
