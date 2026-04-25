# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# test_patterns — single-layer LayerScene demo cycling through display-
# engineering reference patterns (SMPTE bars, gradients, geometry grids).

# Painters carved out so the unit-test binary can link against them
# without rebuilding main.cpp.
add_library(test_patterns_painters STATIC patterns.cpp)
target_link_libraries(test_patterns_painters PUBLIC drm-cxx::drm-cxx)
target_include_directories(test_patterns_painters PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(test_patterns main.cpp)
target_link_libraries(test_patterns PRIVATE drm-cxx::drm-cxx test_patterns_painters)
target_include_directories(test_patterns PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
