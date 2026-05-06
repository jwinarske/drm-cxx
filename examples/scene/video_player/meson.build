# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# video_player — minimal demo of drm::scene::GstAppsinkSource. Only
# built when the project's GStreamer gate is on.

if not has_gstreamer
  subdir_done()
endif

executable('video_player',
  'main.cpp',
  dependencies : [drm_cxx_dep, gst_dep, gst_app_dep],
  include_directories : include_directories('..'),
)
