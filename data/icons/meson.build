# SPDX-License-Identifier: GPL-3.0-or-later

# The scalable and symbolic SVGs install only outside Flatpak: flatpak's
# export step validates every exported hicolor icon with GdkPixbuf
# (flatpak-validate-icon), and Fedora ships no SVG pixbuf loader at all,
# so an exported SVG fails the build with "Format not recognized". The
# 128px PNG below satisfies compose, export, and the Flathub icon
# requirement; GTK scales it for the app grid.
if not get_option('flatpak')
  install_data(
    'hicolor' / 'scalable' / 'apps' / 'io.github.virinvictus.framework.svg',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
  )
  install_data(
    'hicolor' / 'symbolic' / 'apps' / 'io.github.virinvictus.framework-symbolic.svg',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'symbolic' / 'apps',
  )
endif

# Raster icon: installs everywhere. Rendered from the scalable source
# with rsvg-convert; regenerate on icon changes:
#   rsvg-convert -w 128 -h 128 <svg> -o <png>
install_data(
  'hicolor' / '128x128' / 'apps' / 'io.github.virinvictus.framework.png',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / '128x128' / 'apps',
)
