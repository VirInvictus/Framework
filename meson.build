# SPDX-License-Identifier: GPL-3.0-or-later

project('framework', 'c',
  version: '0.79.0',
  meson_version: '>= 1.4',
  default_options: [
    'c_std=c17',
    'warning_level=2',
  ],
)

gnome = import('gnome')
i18n = import('i18n')

config_h = configuration_data()
config_h.set_quoted('APP_ID', 'io.github.virinvictus.framework')
config_h.set_quoted('APP_VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', meson.project_name())
config_h.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
config_h.set_quoted('FW_DATADIR', get_option('prefix') / get_option('datadir'))
config_h.set_quoted('FW_SOURCE_ROOT', meson.project_source_root())
config_h.set10('FW_STRESS_BUILD', get_option('stress'))

# WebKit helper-process directory candidates for the Landlock EXECUTE
# allowlist (fw-sandbox.c). WebKit's .pc exposes no libexecdir variable,
# so derive the two conventional layouts (prefix/libexec on Fedora-style
# distros, libdir on Debian-style) from pkg-config; fw-sandbox.c appends
# hardcoded fallbacks and existence-checks everything at runtime.
webkit_probe = dependency('webkitgtk-6.0', version: '>= 2.46')
webkit_exec_dirs = [
  webkit_probe.get_variable(pkgconfig: 'prefix', default_value: '/usr')
    / 'libexec' / 'webkitgtk-6.0',
]
wk_libdir = webkit_probe.get_variable(pkgconfig: 'libdir', default_value: '')
if wk_libdir != ''
  webkit_exec_dirs += wk_libdir / 'webkitgtk-6.0'
endif
config_h.set_quoted('FW_WEBKIT_EXEC_DIRS', ':'.join(webkit_exec_dirs))

configure_file(
  output: 'fw-config.h',
  configuration: config_h,
)

config_inc = include_directories('.')

add_project_arguments(
  '-I' + meson.project_build_root(),
  language: 'c',
)

subdir('src')
subdir('data')

if get_option('stress')
  subdir('tests')
endif
