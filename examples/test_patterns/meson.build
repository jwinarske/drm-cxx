# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# test_patterns — single-layer LayerScene demo cycling through display-
# engineering reference patterns. The painters carve out as a static
# library so a unit-test binary can link against them without rebuilding
# the example's main loop.

test_patterns_lib = static_library(
  'test_patterns_painters',
  'patterns.cpp',
  dependencies : [drm_cxx_dep],
  include_directories : include_directories('..'),
)

test_patterns_dep = declare_dependency(
  link_with : test_patterns_lib,
  dependencies : [drm_cxx_dep],
  include_directories : include_directories('..'),
)

executable('test_patterns',
  'main.cpp',
  dependencies : [test_patterns_dep],
  include_directories : include_directories('..'),
)
