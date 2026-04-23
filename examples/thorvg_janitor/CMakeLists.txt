# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# Included from examples/CMakeLists.txt only when PkgConfig::thorvg is found.

add_executable(thorvg_janitor tvggame.cpp)
target_link_libraries(thorvg_janitor PRIVATE
  drm-cxx::drm-cxx
  common_scene
  PkgConfig::thorvg)
target_include_directories(thorvg_janitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
