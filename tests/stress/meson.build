# SPDX-License-Identifier: GPL-3.0-or-later

stress_scrub = executable(
  'stress-scrub',
  'stress-scrub.c',
  dependencies: framework_lib_dep,
  install: false,
)

stress_zoom_storm = executable(
  'stress-zoom-storm',
  'stress-zoom-storm.c',
  dependencies: framework_lib_dep,
  install: false,
)

stress_search_cache = executable(
  'stress-search-cache',
  'stress-search-cache.c',
  dependencies: framework_lib_dep,
  install: false,
)

stress_multidoc = executable(
  'stress-multidoc',
  'stress-multidoc.c',
  dependencies: framework_lib_dep,
  install: false,
)

# Register as a meson test. RSS cap of 800 MB by default; override with
# FW_STRESS_RSS_CAP_MB. Skipped automatically in CI environments without a
# corpus path — pass FW_TEST_CORPUS_ROOT or call the binary directly.
corpus_root = '/home/bdkl/docs/Calibre Library'
sample_pdf  = corpus_root / 'Joshua Bloch/Effective Java (5)/Effective Java - Joshua Bloch.pdf'

test('stress-scrub-effective-java',
  stress_scrub,
  args: [sample_pdf],
  env: ['G_TEST_SRCDIR=' + meson.project_source_root(),
        'G_TEST_BUILDDIR=' + meson.project_build_root()],
  timeout: 60,
  protocol: 'exitcode',
  is_parallel: false,
)

test('stress-zoom-storm-effective-java',
  stress_zoom_storm,
  args: [sample_pdf],
  env: ['G_TEST_SRCDIR=' + meson.project_source_root(),
        'G_TEST_BUILDDIR=' + meson.project_build_root()],
  timeout: 90,
  protocol: 'exitcode',
  is_parallel: false,
)

test('stress-search-cache-effective-java',
  stress_search_cache,
  args: [sample_pdf, 'class'],
  env: ['G_TEST_SRCDIR=' + meson.project_source_root(),
        'G_TEST_BUILDDIR=' + meson.project_build_root()],
  timeout: 120,
  protocol: 'exitcode',
  is_parallel: false,
)

test('stress-multidoc',
  stress_multidoc,
  env: ['G_TEST_SRCDIR=' + meson.project_source_root(),
        'G_TEST_BUILDDIR=' + meson.project_build_root()],
  timeout: 180,
  protocol: 'exitcode',
  is_parallel: false,
)
