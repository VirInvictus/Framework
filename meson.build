project('framework', 'c',
  version: '1.0.2',
  meson_version: '>= 1.4',
  default_options: [
    'c_std=c17',
    'warning_level=2',
  ],
)

gnome = import('gnome')
i18n = import('i18n')

config_h = configuration_data()
config_h.set_quoted('APP_ID', 'com.github.vrnvctss.framework')
config_h.set_quoted('APP_VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', meson.project_name())
config_h.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))

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
