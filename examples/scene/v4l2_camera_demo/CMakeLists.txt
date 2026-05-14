# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# v4l2_camera_demo — minimal demo of drm::scene::V4l2CameraSource.

add_executable(v4l2_camera_demo main.cpp)
target_link_libraries(v4l2_camera_demo PRIVATE drm-cxx::drm-cxx)
target_include_directories(v4l2_camera_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
