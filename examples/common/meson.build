# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# examples/common — shared infrastructure used across multiple
# examples. Not installed; lives and dies with the example tree.

common_scene = static_library(
  'common_scene',
  'scene/dumb_buffer_source.cpp',
  'scene/gbm_buffer_source.cpp',
  'scene/layer_scene.cpp',
  dependencies : [drm_cxx_dep],
  include_directories : include_directories('..'),
)

common_scene_dep = declare_dependency(
  link_with : common_scene,
  include_directories : include_directories('..'),
)
