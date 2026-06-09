# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# Examples are organized into four buckets matching docs/scene.md:
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
    allocator/plane_caps
    allocator/compressed_scanout
    scene/layered_demo
    scene/video_grid
    scene/dual_display
    scene/video_wall_multi)
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
  _drm_cxx_promote_system_includes(PkgConfig::libcamera)

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
    set(camera_sources
      scene/camera/main.cpp
      scene/camera/convert.cpp
      scene/camera/double_dumb_source.cpp
      scene/camera/libcamera_nv12_source.cpp
      scene/camera/status_overlay_renderer.cpp)
    # VAAPI MJPEG decode (auto-feature). libva + libva-drm together
    # unlock zero-copy JPEG -> NV12 -> scanout on AMD/Intel; missing
    # either falls the example back to the CPU libyuv path.
    pkg_check_modules(libva IMPORTED_TARGET libva)
    pkg_check_modules(libva_drm IMPORTED_TARGET libva-drm)
    if(TARGET PkgConfig::libva AND TARGET PkgConfig::libva_drm)
      _drm_cxx_promote_system_includes(PkgConfig::libva)
      _drm_cxx_promote_system_includes(PkgConfig::libva_drm)
      list(APPEND camera_sources scene/camera/vaapi_jpeg_decoder.cpp)
    else()
      message(STATUS
        "camera: libva / libva-drm not found, MJPEG falls back to CPU libyuv decode")
    endif()
    add_executable(camera ${camera_sources})
    target_link_libraries(camera PRIVATE drm-cxx::drm-cxx PkgConfig::libcamera drm_cxx_libyuv)
    target_include_directories(camera PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(TARGET PkgConfig::libva AND TARGET PkgConfig::libva_drm)
      target_link_libraries(camera PRIVATE PkgConfig::libva PkgConfig::libva_drm)
      target_compile_definitions(camera PRIVATE CAMERA_HAS_VAAPI=1)
    endif()
    # Status overlay paints a small "fps=N cameras=M" badge via Blend2D
    # when the umbrella gate is on; without it the TU compiles cleanly
    # and main.cpp skips adding the overlay layer entirely.
    if(DRM_CXX_HAS_BLEND2D)
      target_link_libraries(camera PRIVATE blend2d::blend2d)
      target_compile_definitions(camera PRIVATE CAMERA_STATUS_HAS_BLEND2D=1)
    endif()
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

# damage_present — FB_DAMAGE_CLIPS validation harness.
add_executable(damage_present advanced/damage_present/main.cpp)
target_link_libraries(damage_present PRIVATE drm-cxx::drm-cxx)
target_include_directories(damage_present PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# driver_caps — DriverProfile dump (frame-economy caps).
add_executable(driver_caps advanced/driver_caps/main.cpp)
target_link_libraries(driver_caps PRIVATE drm-cxx::drm-cxx)
target_include_directories(driver_caps PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ring_present — DumbRingSource + BufferRing buffer-age validation harness.
add_executable(ring_present advanced/ring_present/main.cpp)
target_link_libraries(ring_present PRIVATE drm-cxx::drm-cxx)
target_include_directories(ring_present PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# idle_present — FrameEconomy idle-suppression harness.
add_executable(idle_present advanced/idle_present/main.cpp)
target_link_libraries(idle_present PRIVATE drm-cxx::drm-cxx)
target_include_directories(idle_present PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# stream_probe — diagnostic CLI for drm::scene::probe_stream_capability.
# Dumps the per-device EGL Streams capability shape so users can verify
# whether their system can host an EglStreamSource. Independent of
# DRM_CXX_HAS_EGL_STREAMS: the binary builds on every host (the probe
# itself short-circuits to Unsupported when EGL headers were absent).
add_executable(stream_probe advanced/stream_probe/main.cpp)
target_link_libraries(stream_probe PRIVATE drm-cxx::drm-cxx)
target_include_directories(stream_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# multi_crtc_probe — enumerate connected outputs and report whether a
# combined-CRTC TEST_ONLY atomic commit is accepted by the kernel.
# Foundational probe for SceneSet-style multi-display work
# (cluster_sim's passenger display, dual_display, video_wall_multi).
# `--hotplug` re-runs the probe on each udev event.
add_executable(multi_crtc_probe advanced/multi_crtc_probe/main.cpp)
target_link_libraries(multi_crtc_probe PRIVATE drm-cxx::drm-cxx)
target_include_directories(multi_crtc_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# stream_demo — end-to-end EGL Streams demo: a stream-source layer
# centered over a dumb-buffer background, fed by a GLES 3 producer
# loop that animates the clear color. Gated on DRM_CXX_HAS_EGL_STREAMS
# (the EglStreamBuilder header isn't defined without it) AND on
# glesv2.pc availability (the demo links real EGL + GLES, no dlopen
# shenanigans — only the library does that). Hardware-only: needs
# the proprietary EGL stack to actually scan a stream out.
if(DRM_CXX_HAS_EGL_STREAMS)
  pkg_check_modules(glesv2 IMPORTED_TARGET glesv2)
  if(TARGET PkgConfig::glesv2 AND TARGET PkgConfig::egl)
    add_executable(stream_demo advanced/stream_demo/main.cpp)
    target_link_libraries(stream_demo PRIVATE
      drm-cxx::drm-cxx
      PkgConfig::egl
      PkgConfig::glesv2)
    target_include_directories(stream_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
endif()

# egl_scene — minimal EGL/GLES 3 producer fronting a GbmSurfaceSource.
# Same EGL + glesv2 gate as stream_demo; the library never links them
# itself.
pkg_check_modules(egl_scene_egl IMPORTED_TARGET egl)
pkg_check_modules(egl_scene_gles IMPORTED_TARGET glesv2)
if(TARGET PkgConfig::egl_scene_egl AND TARGET PkgConfig::egl_scene_gles)
  add_executable(egl_scene advanced/egl_scene/main.cpp)
  target_link_libraries(egl_scene PRIVATE
    drm-cxx::drm-cxx
    PkgConfig::egl_scene_egl
    PkgConfig::egl_scene_gles)
  target_include_directories(egl_scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  # gl_present — GlScanoutProducer demo (EGL/GLES render -> ScanoutBackend).
  add_executable(gl_present advanced/gl_present/main.cpp)
  target_link_libraries(gl_present PRIVATE
    drm-cxx::drm-cxx
    PkgConfig::egl_scene_egl
    PkgConfig::egl_scene_gles)
  target_include_directories(gl_present PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# vk_present — VkScanoutProducer demo. The producer encapsulates Vulkan
# (vulkan.hpp dlopen'd at runtime inside the library), so this links only
# drm-cxx — no libvulkan, no GL. Gated on Vulkan headers being present.
add_executable(vk_present advanced/vk_present/main.cpp)
target_link_libraries(vk_present PRIVATE drm-cxx::drm-cxx)
target_include_directories(vk_present PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# vk_out_fence — validate OUT_FENCE (scanout-done sync_file).
add_executable(vk_out_fence advanced/vk_out_fence/main.cpp)
target_link_libraries(vk_out_fence PRIVATE drm-cxx::drm-cxx)
target_include_directories(vk_out_fence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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

  # mdi_demo — multi-document desktop showcase for drm::csd Tier 0.
  add_subdirectory(mdi_demo)
endif()

if(DRM_CXX_VULKAN)
  add_executable(vulkan_display advanced/vulkan_display/main.cpp)
  target_link_libraries(vulkan_display PRIVATE drm-cxx::drm-cxx)
  target_include_directories(vulkan_display PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  # vulkan_scene — minimal Vulkan-rendered scene layer over
  # VK_EXT_image_drm_format_modifier + ExternalDmaBufSource.
  add_executable(vulkan_scene advanced/vulkan_scene/main.cpp)
  target_link_libraries(vulkan_scene PRIVATE drm-cxx::drm-cxx PkgConfig::vulkan)
  target_include_directories(vulkan_scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  # vulkan_offload_scanout — GPU(Vulkan) render -> dma-buf -> import on the
  # display node (drm::fmt). Same Vulkan gate as vulkan_scene.
  add_executable(vulkan_offload_scanout advanced/vulkan_offload_scanout/main.cpp)
  target_link_libraries(vulkan_offload_scanout PRIVATE drm-cxx::drm-cxx PkgConfig::vulkan)
  target_include_directories(vulkan_offload_scanout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# egl_offload_scanout — GPU(EGL/GLES) render -> dma-buf -> import on the
# display node (drm::fmt). Same EGL + glesv2 gate as egl_scene; the
# library never links these, only the demo's producer side does.
pkg_check_modules(offload_egl IMPORTED_TARGET egl)
pkg_check_modules(offload_gles IMPORTED_TARGET glesv2)
if(TARGET PkgConfig::offload_egl AND TARGET PkgConfig::offload_gles)
  add_executable(egl_offload_scanout advanced/egl_offload_scanout/main.cpp)
  target_link_libraries(egl_offload_scanout PRIVATE
    drm-cxx::drm-cxx
    PkgConfig::offload_egl
    PkgConfig::offload_gles)
  target_include_directories(egl_offload_scanout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  # gbm_surface_scanout — compositor-path counterpart: a gbm_surface swapchain,
  # the only path Mesa hands out displayable-DCC (retile) on amdgpu.
  add_executable(gbm_surface_scanout advanced/gbm_surface_scanout/main.cpp)
  target_link_libraries(gbm_surface_scanout PRIVATE
    drm-cxx::drm-cxx
    PkgConfig::offload_egl
    PkgConfig::offload_gles)
  target_include_directories(gbm_surface_scanout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

add_subdirectory(scene/signage_player)
add_subdirectory(scene/v4l2_camera_demo)
add_subdirectory(scene/video_player)
add_subdirectory(scene/cluster_sim)
add_subdirectory(scene/hdr_demo)
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
  _drm_cxx_promote_system_includes(PkgConfig::thorvg)
  if(TARGET PkgConfig::thorvg)
    add_subdirectory(scene/thorvg_janitor)
  else()
    message(STATUS
      "thorvg_janitor: thorvg not found, skipping "
      "(set DRM_CXX_BUILD_THORVG_JANITOR=ON to hard-fail)")
  endif()
endif()