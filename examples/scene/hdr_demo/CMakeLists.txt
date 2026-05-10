# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# hdr_demo — three-layer LayerScene example exercising the HDR
# signaling, CRTC color pipeline, and CPU tone-map fallback the
# library exposes.

add_executable(hdr_demo main.cpp)
target_link_libraries(hdr_demo PRIVATE drm-cxx::drm-cxx)
target_include_directories(hdr_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
