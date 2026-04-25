# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# signage_player — two-layer LayerScene demo driven by a toml playlist.

tomlplusplus_dep = dependency('tomlplusplus', required : true)

# Playlist parser carved out so test binaries can link against it
# without rebuilding the toml++ wiring.
signage_playlist_lib = static_library(
  'signage_playlist',
  'playlist.cpp',
  dependencies : [drm_cxx_dep, tomlplusplus_dep],
  include_directories : include_directories('..'),
)

signage_playlist_dep = declare_dependency(
  link_with : signage_playlist_lib,
  dependencies : [drm_cxx_dep, tomlplusplus_dep],
  include_directories : include_directories('..'),
)

executable('signage_player',
  'main.cpp',
  dependencies : [drm_cxx_dep, common_scene_dep, signage_playlist_dep],
  include_directories : include_directories('..'),
)
