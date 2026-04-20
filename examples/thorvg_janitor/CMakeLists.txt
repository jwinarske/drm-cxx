# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# Included from examples/CMakeLists.txt only when PkgConfig::thorvg has
# been found. The parent file handles the AUTO/ON/OFF probe; here we just
# build the target.

add_executable(thorvg_janitor main.cpp)
target_link_libraries(thorvg_janitor PRIVATE drm-cxx::drm-cxx PkgConfig::thorvg)
target_include_directories(thorvg_janitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
