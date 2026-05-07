# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# cluster_sim — automotive instrument-cluster showcase. Gated on
# Blend2D because every paint layer (skeleton bg + future dials,
# readouts, warnings) renders via Blend2D. libyuv is optional: when
# present cluster_sim adds a UVC YUYV/MJPEG -> XRGB path for the
# rear-view layer (works on amdgpu DC, which can't import vicodec's
# vmalloc-backed dmabufs); when absent the rear-view falls back to
# the vicodec FWHT test-pattern clip alone.

if not has_blend2d
  subdir_done()
endif

cluster_sim_deps = [drm_cxx_dep, blend2d_dep]
cluster_sim_args = []

cxx = meson.get_compiler('cpp')
cluster_sim_libyuv = cxx.find_library('yuv',
                                      has_headers : ['libyuv.h'],
                                      required : false)
if cluster_sim_libyuv.found()
  cluster_sim_deps += cluster_sim_libyuv
  cluster_sim_args += '-DCLUSTER_SIM_HAS_LIBYUV=1'
endif

executable('cluster_sim',
  'main.cpp',
  dependencies : cluster_sim_deps,
  cpp_args : cluster_sim_args,
  include_directories : include_directories('..'),
)
