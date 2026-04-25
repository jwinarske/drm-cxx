# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

# Shared example infrastructure (scene skeleton, device-selection helpers).
# Must come before any example that links common_scene.
add_subdirectory(common)

foreach(_ex atomic_modeset hotplug_monitor overlay_planes)
  add_executable(${_ex} ${_ex}/main.cpp)
  target_link_libraries(${_ex} PRIVATE drm-cxx::drm-cxx)
  target_include_directories(${_ex} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# mouse_cursor + xcursor_smoke both exercise the library's drm::cursor
# module — no direct libxcursor dep needed here, the library owns that.
add_executable(mouse_cursor mouse_cursor/main.cpp)
target_link_libraries(mouse_cursor PRIVATE drm-cxx::drm-cxx)
target_include_directories(mouse_cursor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(xcursor_smoke mouse_cursor/xcursor_smoke.cpp)
target_link_libraries(xcursor_smoke PRIVATE drm-cxx::drm-cxx)
target_include_directories(xcursor_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# cursor_rotate — V2 hardware-validation harness for drm::cursor.
add_executable(cursor_rotate cursor_rotate/main.cpp)
target_link_libraries(cursor_rotate PRIVATE drm-cxx::drm-cxx)
target_include_directories(cursor_rotate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# capture_demo — libinput-driven snapshot demo for drm::capture.
# Only built when Blend2D is on (same gate as the library module it exercises).
if(DRM_CXX_HAS_BLEND2D)
  add_executable(capture_demo capture_demo/main.cpp)
  target_link_libraries(capture_demo PRIVATE drm-cxx::drm-cxx)
  target_include_directories(capture_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(DRM_CXX_VULKAN)
  add_executable(vulkan_display vulkan_display/main.cpp)
  target_link_libraries(vulkan_display PRIVATE drm-cxx::drm-cxx)
  target_include_directories(vulkan_display PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ThorVG Janitor example — scaffold only (see examples/thorvg_janitor/README.md).
# AUTO = probe thorvg, build if found; ON = hard-require; OFF = skip entirely.
set(DRM_CXX_BUILD_THORVG_JANITOR "AUTO" CACHE STRING
    "Build the ThorVG Janitor example: AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_BUILD_THORVG_JANITOR PROPERTY STRINGS AUTO ON OFF)

add_subdirectory(signage_player)

if(NOT DRM_CXX_BUILD_THORVG_JANITOR STREQUAL "OFF")
  if(DRM_CXX_BUILD_THORVG_JANITOR STREQUAL "ON")
    pkg_check_modules(thorvg REQUIRED IMPORTED_TARGET thorvg-1>=1.0.4)
  else()
    pkg_check_modules(thorvg IMPORTED_TARGET thorvg-1>=1.0.4)
  endif()
  if(TARGET PkgConfig::thorvg)
    add_subdirectory(thorvg_janitor)
  else()
    message(STATUS
      "thorvg_janitor: thorvg not found, skipping "
      "(set DRM_CXX_BUILD_THORVG_JANITOR=ON to hard-fail)")
  endif()
endif()