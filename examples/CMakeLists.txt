# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

foreach(_ex atomic_modeset hotplug_monitor overlay_planes scene_warm_start scene_priority scene_formats layered_demo video_grid)
  add_executable(${_ex} ${_ex}/main.cpp)
  target_link_libraries(${_ex} PRIVATE drm-cxx::drm-cxx)
  target_include_directories(${_ex} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# camera — libcamera + libyuv are hard deps of this example only.
# libyuv (which transitively pulls in libjpeg-turbo for its MJPGToNV12
# path) handles the YUY2/MJPEG-to-NV12 fallback when zero-copy from a
# camera into a scanout plane is unavailable. Their public headers go
# through INTERFACE_SYSTEM_INCLUDE_DIRECTORIES so their warnings don't
# trip our -Werror build (matches libseat / libxcursor / blend2d
# treatment in src/CMakeLists.txt).
pkg_check_modules(libcamera REQUIRED IMPORTED_TARGET libcamera>=0.3.0)
pkg_check_modules(libyuv REQUIRED IMPORTED_TARGET libyuv)
foreach(_dep libcamera libyuv)
  get_target_property(_incs PkgConfig::${_dep} INTERFACE_INCLUDE_DIRECTORIES)
  if(_incs)
    set_property(TARGET PkgConfig::${_dep}
                 APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_incs})
  endif()
endforeach()
add_executable(camera
  camera/main.cpp
  camera/convert.cpp)
target_link_libraries(camera PRIVATE drm-cxx::drm-cxx PkgConfig::libcamera PkgConfig::libyuv)
target_include_directories(camera PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
add_subdirectory(test_patterns)

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