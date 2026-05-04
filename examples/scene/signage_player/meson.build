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
signage_player_args = []
# Overlay text painter uses Blend2D when the project's gate is on.
# Distros (Fedora, Arch) ship blend2d headers under /usr/include
# whether or not we linked against blend2d, so a bare __has_include
# probe in overlay_renderer.cpp is unreliable — it can compile in
# the BL paths while the link line lacks blend2d, which breaks
# `meson setup -Dblend2d=disabled`. Drive the .cpp from the gate
# directly: define SIGNAGE_OVERLAY_HAS_BLEND2D iff we actually link
# blend2d here.
if has_blend2d
  signage_player_deps += blend2d_dep
  signage_player_args += '-DSIGNAGE_OVERLAY_HAS_BLEND2D'
endif

executable('signage_player',
  'main.cpp',
  'overlay_renderer.cpp',
  dependencies : signage_player_deps,
  cpp_args : signage_player_args,
  include_directories : include_directories('..'),
)
