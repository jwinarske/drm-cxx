# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

foreach(_ex atomic_modeset hotplug_monitor overlay_planes scene_warm_start scene_priority scene_formats layered_demo video_grid)
  add_executable(${_ex} ${_ex}/main.cpp)
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
      camera/main.cpp
      camera/convert.cpp)
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