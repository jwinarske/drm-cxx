# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# Examples are organised into four buckets matching docs/scene.md:
#
#   basics/    — minimal LayerScene introductions and small one-feature demos.
#   scene/     — substantial LayerScene workloads (the "sales deck" tier).
#   allocator/ — pedagogical demos for individual planes::Allocator features.
#   advanced/  — demos that exercise non-scene modules (Vulkan, capture, cursor).
#
# Common helpers (open_output, format_probe, vt_switch, select_*) live under
# examples/common/ and are pulled in by sources via "../../common/foo.hpp".

# Single-source examples in basics/, scene/, and allocator/ that need
# nothing beyond drm-cxx itself. Each entry is "<bucket>/<name>"; the
# CMake target name is the basename.
foreach(_path
    basics/atomic_modeset
    basics/hotplug_monitor
    allocator/overlay_planes
    allocator/scene_warm_start
    allocator/scene_priority
    allocator/scene_formats
    scene/layered_demo
    scene/video_grid)
  get_filename_component(_ex ${_path} NAME)
  add_executable(${_ex} ${_path}/main.cpp)
  target_link_libraries(${_ex} PRIVATE drm-cxx::drm-cxx)
  target_include_directories(${_ex} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# camera — libcamera + libyuv are example-only deps. libyuv (which
# transitively pulls in libjpeg-turbo for its MJPGToNV12 path) handles
# the YUY2/MJPEG-to-NV12 fallback when zero-copy from a camera into a
# scanout plane is unavailable. AUTO = probe and build if both found;
# ON = hard-require; OFF = skip. Ubuntu 24.04 ships libcamera 0.2.0,
# so AUTO silently skips on stock CI runners. Public headers go
# through INTERFACE_SYSTEM_INCLUDE_DIRECTORIES so their warnings don't
# trip our -Werror build (matches libseat / libxcursor / blend2d
# treatment in src/CMakeLists.txt).
set(DRM_CXX_BUILD_CAMERA "AUTO" CACHE STRING
    "Build the camera example: AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_BUILD_CAMERA PROPERTY STRINGS AUTO ON OFF)

if(NOT DRM_CXX_BUILD_CAMERA STREQUAL "OFF")
  if(DRM_CXX_BUILD_CAMERA STREQUAL "ON")
    pkg_check_modules(libcamera REQUIRED IMPORTED_TARGET libcamera>=0.3.0)
  else()
    pkg_check_modules(libcamera IMPORTED_TARGET libcamera>=0.3.0)
  endif()

  # libyuv is detected manually rather than via pkg-config: Fedora's
  # libyuv-devel ships a libyuv.pc but Ubuntu/Debian's libyuv-dev does
  # not, so the pkg-config probe is unreliable. find_path / find_library
  # works on both distros (header at /usr/include/libyuv.h, lib at
  # /usr/lib*/libyuv.so).
  find_path(LIBYUV_INCLUDE_DIR NAMES libyuv.h)
  find_library(LIBYUV_LIBRARY NAMES yuv)
  if(LIBYUV_INCLUDE_DIR AND LIBYUV_LIBRARY)
    add_library(drm_cxx_libyuv INTERFACE)
    target_include_directories(drm_cxx_libyuv SYSTEM INTERFACE ${LIBYUV_INCLUDE_DIR})
    target_link_libraries(drm_cxx_libyuv INTERFACE ${LIBYUV_LIBRARY})
  elseif(DRM_CXX_BUILD_CAMERA STREQUAL "ON")
    message(FATAL_ERROR
      "camera: libyuv not found (libyuv-dev on Debian/Ubuntu, "
      "libyuv-devel on Fedora). Set DRM_CXX_BUILD_CAMERA=OFF to skip.")
  endif()

  if(TARGET PkgConfig::libcamera AND TARGET drm_cxx_libyuv)
    get_target_property(_incs PkgConfig::libcamera INTERFACE_INCLUDE_DIRECTORIES)
    if(_incs)
      set_property(TARGET PkgConfig::libcamera
                   APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_incs})
    endif()
    add_executable(camera
      scene/camera/main.cpp
      scene/camera/convert.cpp)
    target_link_libraries(camera PRIVATE drm-cxx::drm-cxx PkgConfig::libcamera drm_cxx_libyuv)
    target_include_directories(camera PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  else()
    message(STATUS
      "camera: libcamera>=0.3.0 or libyuv not found, skipping "
      "(set DRM_CXX_BUILD_CAMERA=ON to hard-fail)")
  endif()
endif()

# mouse_cursor + xcursor_smoke both exercise the library's drm::cursor
# module — no direct libxcursor dep needed here, the library owns that.
add_executable(mouse_cursor basics/mouse_cursor/main.cpp)
target_link_libraries(mouse_cursor PRIVATE drm-cxx::drm-cxx)
target_include_directories(mouse_cursor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(xcursor_smoke basics/mouse_cursor/xcursor_smoke.cpp)
target_link_libraries(xcursor_smoke PRIVATE drm-cxx::drm-cxx)
target_include_directories(xcursor_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# cursor_rotate — V2 hardware-validation harness for drm::cursor.
add_executable(cursor_rotate advanced/cursor_rotate/main.cpp)
target_link_libraries(cursor_rotate PRIVATE drm-cxx::drm-cxx)
target_include_directories(cursor_rotate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# capture_demo — libinput-driven snapshot demo for drm::capture.
# Only built when Blend2D is on (same gate as the library module it exercises).
if(DRM_CXX_HAS_BLEND2D)
  add_executable(capture_demo advanced/capture_demo/main.cpp)
  target_link_libraries(capture_demo PRIVATE drm-cxx::drm-cxx)
  target_include_directories(capture_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  # csd_smoke — throwaway hardware-validation harness for drm::csd.
  add_executable(csd_smoke advanced/csd_smoke/main.cpp)
  target_link_libraries(csd_smoke PRIVATE drm-cxx::drm-cxx)
  target_include_directories(csd_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(DRM_CXX_VULKAN)
  add_executable(vulkan_display advanced/vulkan_display/main.cpp)
  target_link_libraries(vulkan_display PRIVATE drm-cxx::drm-cxx)
  target_include_directories(vulkan_display PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

add_subdirectory(scene/signage_player)
add_subdirectory(basics/test_patterns)

# keyboard — interactive keyboard-input + Unicode-entry demo.
# Default OFF because the configure step downloads Noto Sans, which
# does not work in sandboxed builds (Yocto, OE, distro packagers).
# Set DRM_CXX_BUILD_KEYBOARD=ON when network access is available
# at configure time and Blend2D is present.
set(DRM_CXX_BUILD_KEYBOARD "OFF" CACHE STRING
    "Build the keyboard demo (downloads Noto Sans at configure time): ON or OFF")
set_property(CACHE DRM_CXX_BUILD_KEYBOARD PROPERTY STRINGS ON OFF)

if(DRM_CXX_BUILD_KEYBOARD)
  if(NOT DRM_CXX_HAS_BLEND2D)
    message(FATAL_ERROR
      "keyboard demo needs Blend2D (DRM_CXX_BLEND2D=ON); "
      "set DRM_CXX_BUILD_KEYBOARD=OFF to skip.")
  endif()
  add_subdirectory(basics/keyboard)
endif()

# ThorVG Janitor example — scaffold only (see scene/thorvg_janitor/README.md).
# AUTO = probe thorvg, build if found; ON = hard-require; OFF = skip entirely.
set(DRM_CXX_BUILD_THORVG_JANITOR "AUTO" CACHE STRING
    "Build the ThorVG Janitor example: AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_BUILD_THORVG_JANITOR PROPERTY STRINGS AUTO ON OFF)

if(NOT DRM_CXX_BUILD_THORVG_JANITOR STREQUAL "OFF")
  if(DRM_CXX_BUILD_THORVG_JANITOR STREQUAL "ON")
    pkg_check_modules(thorvg REQUIRED IMPORTED_TARGET thorvg-1>=1.0.4)
  else()
    pkg_check_modules(thorvg IMPORTED_TARGET thorvg-1>=1.0.4)
  endif()
  if(TARGET PkgConfig::thorvg)
    add_subdirectory(scene/thorvg_janitor)
  else()
    message(STATUS
      "thorvg_janitor: thorvg not found, skipping "
      "(set DRM_CXX_BUILD_THORVG_JANITOR=ON to hard-fail)")
  endif()
endif()