# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# signage_player — four-layer LayerScene demo driven by a toml playlist.

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

signage_player_deps = [drm_cxx_dep, signage_playlist_dep]
# Overlay text painter uses Blend2D when available; the .cpp's
# __has_include probe degrades to a bg-only fill when it isn't. The
# top-level meson.build already runs the blend2d probe; here we just
# gate the link dependency.
if has_blend2d
  signage_player_deps += blend2d_dep
endif

executable('signage_player',
  'main.cpp',
  'overlay_renderer.cpp',
  dependencies : signage_player_deps,
  include_directories : include_directories('..'),
)
