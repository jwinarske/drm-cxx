# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# mdi_demo — multi-document desktop on top of drm::csd. Tier 0 plane-
# per-decoration presentation, glass theme. Built only when the
# Blend2D gate is on (csd lives behind it).

add_executable(mdi_demo main.cpp shell.cpp)
target_link_libraries(mdi_demo PRIVATE drm-cxx::drm-cxx)
target_include_directories(mdi_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)