# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# cluster_sim — automotive instrument-cluster showcase. Gated on
# Blend2D because every paint layer (skeleton bg + future dials,
# readouts, warnings) renders via Blend2D.

if not has_blend2d
  subdir_done()
endif

executable('cluster_sim',
  'main.cpp',
  dependencies : [drm_cxx_dep, blend2d_dep],
  include_directories : include_directories('..'),
)
