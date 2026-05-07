# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# cluster_sim — automotive instrument-cluster showcase. Gated on
# Blend2D because every paint layer (bg + dials, readouts, warnings)
# renders via Blend2D. libyuv is optional: when present cluster_sim
# adds a UVC YUYV/MJPEG -> XRGB path for the rear-view layer (works on
# amdgpu DC, which can't import vicodec's vmalloc-backed dmabufs);
# when absent the rear-view falls back to the vicodec FWHT test-pattern
# clip alone.

if(NOT DRM_CXX_HAS_BLEND2D)
  return()
endif()

add_executable(cluster_sim main.cpp)
target_link_libraries(cluster_sim PRIVATE
  drm-cxx::drm-cxx
  blend2d::blend2d)
target_include_directories(cluster_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_library(CLUSTER_SIM_LIBYUV NAMES yuv)
find_path(CLUSTER_SIM_LIBYUV_INCLUDE NAMES libyuv.h)
if(CLUSTER_SIM_LIBYUV AND CLUSTER_SIM_LIBYUV_INCLUDE)
  target_link_libraries(cluster_sim PRIVATE ${CLUSTER_SIM_LIBYUV})
  target_include_directories(cluster_sim SYSTEM PRIVATE ${CLUSTER_SIM_LIBYUV_INCLUDE})
  target_compile_definitions(cluster_sim PRIVATE CLUSTER_SIM_HAS_LIBYUV=1)
endif()
