# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# video_player — minimal demo of drm::scene::GstAppsinkSource. Only
# built when the project's GStreamer gate is on (DRM_CXX_GSTREAMER ≠ OFF
# and the four gstreamer-* pkg-config packages were located).

if(NOT DRM_CXX_HAS_GSTREAMER)
  return()
endif()

add_executable(video_player main.cpp)
target_link_libraries(video_player PRIVATE
  drm-cxx::drm-cxx
  PkgConfig::gstreamer
  PkgConfig::gstreamer_app)
target_include_directories(video_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
