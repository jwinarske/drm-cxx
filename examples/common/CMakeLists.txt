# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# examples/common — shared infrastructure used across multiple
# examples. Not installed; lives and dies with the example tree.
#
# Today: drm::scene skeleton (M2 Phase 2.1) plus the small helper
# headers like select_device.hpp. Graduates to src/scene/ once the
# abstraction has proved itself across two consumers.

add_library(common_scene STATIC
  scene/dumb_buffer_source.cpp
  scene/layer_scene.cpp)

target_link_libraries(common_scene PUBLIC drm-cxx::drm-cxx)

target_include_directories(common_scene
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
