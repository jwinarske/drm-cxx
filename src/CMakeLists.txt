# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

set(DRM_CXX_SOURCES
  core/device.cpp
  core/resources.cpp
  core/property_store.cpp
  core/format.cpp
  modeset/atomic.cpp
  modeset/mode.cpp
  modeset/modeset.cpp
  modeset/page_flip.cpp
  planes/plane_registry.cpp
  planes/layer.cpp
  planes/output.cpp
  planes/allocator.cpp
  planes/matching.cpp
  input/seat.cpp
  input/keyboard.cpp
  input/key_repeater.cpp
  input/pointer.cpp
  input/event_dispatcher.cpp
  display/color_pipeline_curves.cpp
  display/connector_capabilities.cpp
  display/crtc_capabilities.cpp
  display/crtc_color_pipeline.cpp
  display/edid.cpp
  display/hdr_metadata.cpp
  display/tone_mapper.cpp
  display/hdr_metadata_cache.cpp
  display/hotplug_monitor.cpp
  display/scanout_target.cpp
  display/driver_profile.cpp
  display/mode_list.cpp
  session/seat.cpp
  time/clock.cpp
  gbm/device.cpp
  gbm/surface.cpp
  gbm/buffer.cpp
  dumb/buffer.cpp
  scene/composite_canvas.cpp
  scene/dumb_buffer_source.cpp
  core/egl_loader.cpp
  core/gles_loader.cpp
  scene/gl_compositor.cpp
  scene/egl_stream_builder.cpp
  scene/egl_stream_source.cpp
  scene/external_dma_buf_source.cpp
  scene/dma_buf_source_cache.cpp
  scene/gbm_buffer_source.cpp
  scene/gbm_surface_source.cpp
  scene/gst_appsink_source.cpp
  scene/layer_scene.cpp
  scene/output_signaling.cpp
  scene/scene_set.cpp
  scene/stream_capability.cpp
  scene/v4l2_camera_source.cpp
  scene/v4l2_decoder_source.cpp
  sync/fence.cpp
  fmt/format_mod.cpp
  present/negotiate.cpp
  present/buffer_ring.cpp
  present/dumb_ring_source.cpp
  present/dumb_scanout_sink.cpp
  present/scanout_format.cpp
  present/gbm_producer.cpp
  present/scanout_backend.cpp
  present/gl_scanout_producer.cpp
  present/vk_scanout_producer.cpp)

if(DRM_CXX_VULKAN)
  list(APPEND DRM_CXX_SOURCES
    vulkan/display.cpp
    vulkan/drm_surface.cpp)
endif()

if(DRM_CXX_HAS_XCURSOR)
  list(APPEND DRM_CXX_SOURCES
    cursor/theme.cpp
    cursor/cursor.cpp
    cursor/renderer.cpp
    # Scene source presenting a cursor as an ordinary layer; depends on
    # drm::cursor, so gated with it.
    scene/cursor_source.cpp
    # Vendored X11-free .xcursor loader (replaces system libxcursor).
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/xcursor-mini/xcursor_file.c)
endif()

if(DRM_CXX_HAS_BLEND2D)
  list(APPEND DRM_CXX_SOURCES
    capture/snapshot.cpp
    capture/png.cpp
    csd/theme.cpp
    csd/surface.cpp
    csd/shadow_cache.cpp
    csd/renderer.cpp
    csd/animator.cpp
    csd/overlay_reservation.cpp
    csd/presenter_plane.cpp)
endif()

include(GNUInstallDirs)

add_library(drm-cxx ${DRM_CXX_SOURCES})
add_library(drm-cxx::drm-cxx ALIAS drm-cxx)

set_target_properties(drm-cxx PROPERTIES
  VERSION     ${PROJECT_VERSION}
  SOVERSION   ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME drm-cxx)

# Two public include roots:
#   - src/ so internal cross-subdir includes like "core/foo.hpp" resolve
#     (matches meson's include_directories('.') on the source subdir).
#   - ${binary}/include so public headers use <drm-cxx/...>, served by a
#     build-tree symlink drm-cxx -> src that the root CMakeLists creates.
#     The installed layout still uses the real ${includedir}/drm-cxx dir.
target_include_directories(drm-cxx
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/vendor>
  PRIVATE
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>)

target_compile_features(drm-cxx PUBLIC cxx_std_17)

target_compile_options(drm-cxx PRIVATE
  -Wall -Wextra -Wpedantic
  -Wno-unused-parameter
  -Wno-missing-field-initializers)

target_link_libraries(drm-cxx
  PUBLIC
    PkgConfig::drm
    PkgConfig::gbm
    PkgConfig::libinput
    PkgConfig::libudev
    PkgConfig::xkbcommon
    PkgConfig::libdisplayinfo
  PRIVATE
    ${RT_LIBRARY}
    ${DL_LIBRARY})

if(HAVE_GBM_BO_CREATE_WITH_MODIFIERS2)
  target_compile_definitions(drm-cxx PRIVATE HAVE_GBM_BO_CREATE_WITH_MODIFIERS2=1)
endif()

# libseat is PRIVATE — consumers of drm-cxx don't need to link against
# it themselves. The gate define is PRIVATE too so the session/seat.cpp
# TU picks the real vs. stub branch; the public header compiles
# identically either way.
if(DRM_CXX_HAS_LIBSEAT)
  get_target_property(_libseat_incs PkgConfig::libseat INTERFACE_INCLUDE_DIRECTORIES)
  if(_libseat_incs)
    set_property(TARGET PkgConfig::libseat
                 APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_libseat_incs})
  endif()
  target_link_libraries(drm-cxx PRIVATE PkgConfig::libseat)
  target_compile_definitions(drm-cxx PRIVATE DRM_CXX_HAS_LIBSEAT=1)
else()
  target_compile_definitions(drm-cxx PRIVATE DRM_CXX_HAS_LIBSEAT=0)
  message(STATUS
    "drm-cxx: libseat not found — drm::session::Seat::open() will return nullopt "
    "at runtime (set DRM_CXX_SESSION=ON to hard-fail)")
endif()

# GStreamer: same dual-branch pattern as libseat. Headers promoted to
# -isystem so glib-2.0 / gstreamer warnings stay out of -Werror.
if(DRM_CXX_HAS_GSTREAMER)
  foreach(_gst_tgt PkgConfig::gstreamer PkgConfig::gstreamer_app
                   PkgConfig::gstreamer_alloc PkgConfig::gstreamer_video)
    get_target_property(_gst_incs ${_gst_tgt} INTERFACE_INCLUDE_DIRECTORIES)
    if(_gst_incs)
      set_property(TARGET ${_gst_tgt}
                   APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_gst_incs})
    endif()
  endforeach()
  target_link_libraries(drm-cxx PRIVATE
    PkgConfig::gstreamer
    PkgConfig::gstreamer_app
    PkgConfig::gstreamer_alloc
    PkgConfig::gstreamer_video)
  target_compile_definitions(drm-cxx PRIVATE DRM_CXX_HAS_GSTREAMER=1)
else()
  target_compile_definitions(drm-cxx PRIVATE DRM_CXX_HAS_GSTREAMER=0)
endif()

# EGL Streams — headers-only. libEGL must NOT be linked: the .cpp
# dlopen's "libEGL.so.1" at runtime so drm-cxx stays loadable on Mesa
# systems that have egl.h installed but no libEGL.so.1 (or no NVIDIA
# stack at all). PkgConfig::egl carries -lEGL on most distros, so we
# only adopt its include dirs rather than linking it.
# DRM_CXX_HAS_EGL (render-offload + GPU composition) and DRM_CXX_HAS_EGL_STREAMS
# are computed independently in the root CMakeLists (streams implies egl). Adopt
# EGL include dirs when either is on; libEGL/libGLESv2 stay dlopen-only.
if(DRM_CXX_HAS_EGL)
  if(TARGET PkgConfig::egl)
    get_target_property(_egl_incs PkgConfig::egl INTERFACE_INCLUDE_DIRECTORIES)
    if(_egl_incs)
      target_include_directories(drm-cxx SYSTEM PRIVATE ${_egl_incs})
    endif()
  endif()
endif()
target_compile_definitions(drm-cxx PUBLIC
  DRM_CXX_HAS_EGL_STREAMS=${DRM_CXX_HAS_EGL_STREAMS}
  DRM_CXX_HAS_EGL=${DRM_CXX_HAS_EGL})

# Vulkan headers for VkScanoutProducer. vulkan.hpp's dynamic dispatcher dlopen's
# libvulkan at runtime, so we adopt its include dirs only and never add it to
# drm-cxx's link line (mirrors EGL). Probed independently of DRM_CXX_VULKAN.
# Vulkan headers from the vendored submodule (third_party/Vulkan-Headers) so
# vulkan.hpp is identical everywhere; libvulkan is dlopen'd at runtime, so
# DRM_CXX_HAS_VULKAN is always on. BEFORE so the vendored vulkan.hpp wins over
# any system copy the linked PkgConfig::vulkan (display path) would add.
target_include_directories(drm-cxx SYSTEM BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/Vulkan-Headers/include)
target_compile_definitions(drm-cxx PUBLIC DRM_CXX_HAS_VULKAN=1)

# Vendored X11-free .xcursor loader (third_party/xcursor-mini): its
# xcursor_file.c is compiled straight into drm-cxx, so there is no library
# to link — just put its header on the private include path. -isystem keeps
# any warnings out of our -Werror build, matching the old libxcursor pattern.
if(DRM_CXX_HAS_XCURSOR)
  target_include_directories(drm-cxx SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/xcursor-mini)
  if(DRM_CXX_HAS_XCURSOR_DELAY)
    target_compile_definitions(drm-cxx PRIVATE HAS_XCURSOR_DELAY)
  endif()
endif()

# Blend2D: PRIVATE link + -isystem so its public headers (blend2d.h,
# blend2d/*.h) don't trip clang-tidy or -Werror. Matches libseat /
# libxcursor / fmt treatment.
if(DRM_CXX_HAS_BLEND2D)
  get_target_property(_blend2d_incs blend2d::blend2d INTERFACE_INCLUDE_DIRECTORIES)
  if(_blend2d_incs)
    set_property(TARGET blend2d::blend2d
                 APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_blend2d_incs})
  endif()
  target_link_libraries(drm-cxx PRIVATE blend2d::blend2d)
  target_compile_definitions(drm-cxx PRIVATE DRM_CXX_HAS_BLEND2D=1)
  # toml++ is header-only, PRIVATE link, included as -isystem so its
  # generated diagnostics never reach our -Werror build.
  target_link_libraries(drm-cxx PRIVATE tomlplusplus::tomlplusplus)
else()
  target_compile_definitions(drm-cxx PRIVATE DRM_CXX_HAS_BLEND2D=0)
endif()

if(DRM_CXX_VULKAN)
  target_link_libraries(drm-cxx PUBLIC PkgConfig::vulkan)
endif()

if(NOT DRM_CXX_HAS_STD_PRINT)
  if(TARGET PkgConfig::fmt)
    target_link_libraries(drm-cxx PUBLIC PkgConfig::fmt)
  else()
    target_link_libraries(drm-cxx PUBLIC fmt::fmt)
  endif()
endif()

# C++17 polyfills — linked PUBLIC so the tl/tcb include paths propagate
# into this target's build-tree usage requirements through the detail/
# adapter headers. Install-interface fallout is handled below by vendoring
# the headers under ${includedir}/drm-cxx/vendor so downstream consumers
# of the installed package don't need the polyfill targets.
if(NOT DRM_CXX_HAS_STD_EXPECTED)
  target_link_libraries(drm-cxx PUBLIC $<BUILD_INTERFACE:tl::expected>)
endif()
if(NOT DRM_CXX_HAS_STD_SPAN)
  target_link_libraries(drm-cxx PUBLIC $<BUILD_INTERFACE:tcb::span>)
endif()

# ---------------------------------------------------------------------------
# drm-cxx::cursor-load — the XCursor loader (Theme + Cursor) WITHOUT the
# display stack. Compiles only cursor/theme.cpp + cursor/cursor.cpp + the
# vendored X11-free .xcursor loader; it links nothing beyond the standard
# library (or the header-only span/expected polyfill under C++17). It
# deliberately omits cursor/renderer.cpp and scene/cursor_source.cpp — the
# parts that pull in libdrm/gbm/KMS — so a consumer can resolve a theme and
# rasterize cursor frames into its own ARGB8888 buffers without dragging in
# the whole DRM display dependency chain. Built alongside drm-cxx whenever
# the cursor feature is on.
# ---------------------------------------------------------------------------
if(DRM_CXX_HAS_XCURSOR)
  add_library(cursor-load STATIC
    cursor/theme.cpp
    cursor/cursor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/xcursor-mini/xcursor_file.c)
  add_library(drm-cxx::cursor-load ALIAS cursor-load)

  set_target_properties(cursor-load PROPERTIES
    # libdrm-cxx-cursor-load.a — namespaced so the installed archive can't
    # collide with an unrelated libcursor-load, and matches the meson name.
    OUTPUT_NAME               drm-cxx-cursor-load
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME               cursor-load)

  # Same two public include roots as drm-cxx (see above): the build-tree
  # drm-cxx -> src symlink for <drm-cxx/...>, src/ for internal "detail/..."
  # includes, and the install-tree dirs.
  target_include_directories(cursor-load
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx>)

  # The vendored span/expected polyfills only install (under drm-cxx/vendor)
  # when the toolchain lacks the std versions. Reference that include dir in
  # the install interface only then — otherwise CMake errors on an imported
  # target pointing at a directory the std-path install never created.
  if(NOT DRM_CXX_HAS_STD_EXPECTED OR NOT DRM_CXX_HAS_STD_SPAN)
    target_include_directories(cursor-load PUBLIC
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/vendor>)
  endif()

  target_compile_features(cursor-load PUBLIC cxx_std_17)

  target_compile_options(cursor-load PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-missing-field-initializers)

  # Vendored loader header on the private -isystem path (matches drm-cxx).
  target_include_directories(cursor-load SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/xcursor-mini)
  if(DRM_CXX_HAS_XCURSOR_DELAY)
    target_compile_definitions(cursor-load PRIVATE HAS_XCURSOR_DELAY)
  endif()

  # Header-only C++17 polyfills, build-interface only — the install tree
  # ships vendored copies under drm-cxx/vendor (handled below), exactly as
  # for drm-cxx, so downstream consumers need no polyfill packages.
  if(NOT DRM_CXX_HAS_STD_EXPECTED)
    target_link_libraries(cursor-load PUBLIC $<BUILD_INTERFACE:tl::expected>)
  endif()
  if(NOT DRM_CXX_HAS_STD_SPAN)
    target_link_libraries(cursor-load PUBLIC $<BUILD_INTERFACE:tcb::span>)
  endif()
endif()

if(NOT DRM_CXX_INSTALL)
  return()
endif()

include(CMakePackageConfigHelpers)

install(FILES
    buffer_mapping.hpp
    drm-cxx.hpp
    log.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx)

install(FILES
    detail/expected.hpp
    detail/format.hpp
    detail/span.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/detail)

install(FILES
    core/device.hpp
    core/resources.hpp
    core/property_store.hpp
    core/format.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/core)

install(FILES
    modeset/atomic.hpp
    modeset/mode.hpp
    modeset/modeset.hpp
    modeset/page_flip.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/modeset)

install(FILES
    planes/plane_registry.hpp
    planes/layer.hpp
    planes/output.hpp
    planes/allocator.hpp
    planes/matching.hpp
    planes/composition_layer.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/planes)

install(FILES
    input/seat.hpp
    input/keyboard.hpp
    input/key_repeater.hpp
    input/pointer.hpp
    input/event_dispatcher.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/input)

install(FILES
    display/color_pipeline_curves.hpp
    display/connector_capabilities.hpp
    display/connector_info.hpp
    display/crtc_capabilities.hpp
    display/crtc_color_pipeline.hpp
    display/edid.hpp
    display/hdr_metadata.hpp
    display/hdr_metadata_cache.hpp
    display/hotplug_monitor.hpp
    display/scanout_target.hpp
    display/driver_profile.hpp
    display/mode_list.hpp
    display/tone_mapper.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/display)

install(FILES
    session/seat.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/session)

install(FILES
    time/clock.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/time)

if(DRM_CXX_HAS_XCURSOR)
  install(FILES
      cursor/theme.hpp
      cursor/cursor.hpp
      cursor/renderer.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/cursor)
endif()

if(DRM_CXX_HAS_BLEND2D)
  install(FILES
      capture/snapshot.hpp
      capture/png.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/capture)
  install(FILES
      csd/theme.hpp
      csd/surface.hpp
      csd/window_state.hpp
      csd/shadow_cache.hpp
      csd/renderer.hpp
      csd/animator.hpp
      csd/overlay_reservation.hpp
      csd/presenter.hpp
      csd/presenter_plane.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/csd)
endif()

install(FILES
    gbm/device.hpp
    gbm/surface.hpp
    gbm/buffer.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/gbm)

install(FILES sync/fence.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/sync)

install(FILES dumb/buffer.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/dumb)

install(FILES fmt/format_mod.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/fmt)

install(FILES present/negotiate.hpp present/scanout_producer.hpp present/buffer_ring.hpp
        present/dumb_ring_source.hpp present/dumb_scanout_sink.hpp present/scanout_format.hpp present/frame_economy.hpp
        present/gbm_producer.hpp present/scanout_backend.hpp present/gl_scanout_producer.hpp
        present/vk_scanout_producer.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/present)

install(FILES
    scene/buffer_source.hpp
    scene/commit_report.hpp
    scene/compatibility_report.hpp
    scene/composite_canvas.hpp
    scene/composition_target.hpp
    scene/display_params.hpp
    scene/dumb_buffer_source.hpp
    scene/external_dma_buf_source.hpp
    scene/dma_buf_source_cache.hpp
    scene/gbm_buffer_source.hpp
    scene/gbm_surface_source.hpp
    scene/layer.hpp
    scene/layer_desc.hpp
    scene/layer_handle.hpp
    scene/layer_scene.hpp
    scene/output_signaling.hpp
    scene/scene_set.hpp
    scene/stream_capability.hpp
    scene/v4l2_camera_source.hpp
    scene/v4l2_decoder_source.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/scene)

# GstAppsinkSource header only installs when the gate is on — matches
# the capture / csd pattern.
if(DRM_CXX_HAS_GSTREAMER)
  install(FILES scene/gst_appsink_source.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/scene)
endif()

# EglStreamBuilder + EglStreamSource headers only install when the
# streams gate is on — both pull in EGL/egl.h and would fail to
# compile on hosts without EGL development headers. The source
# header is part of the public surface so callers can query
# `producer_surface()` after the scene's first commit triggers the
# deferred producer-side attachment.
if(DRM_CXX_HAS_EGL_STREAMS)
  install(FILES
      scene/egl_stream_builder.hpp
      scene/egl_stream_source.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/scene)
endif()

if(DRM_CXX_VULKAN)
  install(FILES
      vulkan/display.hpp
      vulkan/drm_surface.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/vulkan)
endif()

# Vendor polyfill headers into the install tree so downstream consumers
# don't need tl-expected / tcb-span installed separately. The detail/
# adapter's `#include <tl/expected.hpp>` / `#include <tcb/span.hpp>`
# resolves through the ${includedir}/drm-cxx/vendor install-interface
# include dir.
if(NOT DRM_CXX_HAS_STD_EXPECTED)
  install(FILES "${PROJECT_SOURCE_DIR}/subprojects/tl-expected/include/tl/expected.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/vendor/tl)
endif()
if(NOT DRM_CXX_HAS_STD_SPAN)
  install(FILES "${PROJECT_SOURCE_DIR}/subprojects/tcb-span/include/tcb/span.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/vendor/tcb)
endif()

install(TARGETS drm-cxx
  EXPORT  drm-cxx-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# cursor-load rides the same export set so find_package(drm-cxx) yields
# drm-cxx::cursor-load. Its public headers (cursor/theme.hpp, cursor/cursor.hpp)
# already install under the DRM_CXX_HAS_XCURSOR block above; the vendored
# span/expected polyfills install under drm-cxx/vendor below.
if(DRM_CXX_HAS_XCURSOR)
  install(TARGETS cursor-load
    EXPORT  drm-cxx-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

  # Standalone pkg-config so non-CMake consumers can link the loader alone
  # without pulling drm-cxx.pc's drm/gbm/input Requires.
  configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/drm-cxx-cursor-load.pc.in"
    "${PROJECT_BINARY_DIR}/drm-cxx-cursor-load.pc"
    @ONLY)
  install(FILES "${PROJECT_BINARY_DIR}/drm-cxx-cursor-load.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

install(EXPORT drm-cxx-targets
  FILE        drm-cxx-targets.cmake
  NAMESPACE   drm-cxx::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/drm-cxx)

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/drm-cxx-config.cmake.in"
  "${PROJECT_BINARY_DIR}/drm-cxx-config.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/drm-cxx)

write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/drm-cxx-config-version.cmake"
  VERSION       ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
    "${PROJECT_BINARY_DIR}/drm-cxx-config.cmake"
    "${PROJECT_BINARY_DIR}/drm-cxx-config-version.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/drm-cxx)

if(DRM_CXX_VULKAN)
  set(PC_VULKAN_REQUIRES " vulkan")
else()
  set(PC_VULKAN_REQUIRES "")
endif()

if(DRM_CXX_HAS_STD_PRINT)
  set(PC_FMT_REQUIRES "")
elseif(TARGET PkgConfig::fmt)
  # fmt came from pkg-config — safe to list as a Requires so raw
  # `pkg-config --libs drm-cxx` picks it up transitively.
  set(PC_FMT_REQUIRES " fmt")
else()
  # fmt was FetchContent'd or came from find_package(fmt CONFIG); it has
  # no pkg-config file installed, so listing it in Requires would make
  # downstream `pkg-config drm-cxx` fail. Fall back to a private -lfmt
  # link, which is sufficient because fmt was FMT_INSTALL=ON'd into our
  # prefix alongside libdrm-cxx.
  set(PC_FMT_REQUIRES "")
  set(PC_FMT_LIBS_PRIVATE " -lfmt")
endif()
if(NOT DEFINED PC_FMT_LIBS_PRIVATE)
  set(PC_FMT_LIBS_PRIVATE "")
endif()

configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/drm-cxx.pc.in"
  "${PROJECT_BINARY_DIR}/drm-cxx.pc"
  @ONLY)

install(FILES "${PROJECT_BINARY_DIR}/drm-cxx.pc"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
