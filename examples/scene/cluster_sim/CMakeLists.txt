# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# cluster_sim — automotive instrument-cluster showcase. Gated on
# Blend2D because the dial / readout / warning layers (landing in
# follow-up commits) all paint via Blend2D; the skeleton's bg gradient
# is also Blend2D-painted.

if(NOT DRM_CXX_HAS_BLEND2D)
  return()
endif()

add_executable(cluster_sim main.cpp)
target_link_libraries(cluster_sim PRIVATE
  drm-cxx::drm-cxx
  blend2d::blend2d)
target_include_directories(cluster_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
