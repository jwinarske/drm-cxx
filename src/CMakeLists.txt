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
  input/pointer.cpp
  input/event_dispatcher.cpp
  display/edid.cpp
  display/hotplug_monitor.cpp
  session/seat.cpp
  time/clock.cpp
  gbm/device.cpp
  gbm/surface.cpp
  gbm/buffer.cpp
  sync/fence.cpp)

if(DRM_CXX_VULKAN)
  list(APPEND DRM_CXX_SOURCES
    vulkan/display.cpp
    vulkan/drm_surface.cpp)
endif()

if(DRM_CXX_HAS_XCURSOR)
  list(APPEND DRM_CXX_SOURCES
    cursor/theme.cpp
    cursor/cursor.cpp
    cursor/renderer.cpp)
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

# libxcursor: linked PRIVATE and included as -isystem so its public
# header warnings don't hit our -Werror build. Matches the libseat
# pattern.
if(DRM_CXX_HAS_XCURSOR)
  get_target_property(_xcursor_incs PkgConfig::xcursor INTERFACE_INCLUDE_DIRECTORIES)
  if(_xcursor_incs)
    set_property(TARGET PkgConfig::xcursor
                 APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_xcursor_incs})
  endif()
  target_link_libraries(drm-cxx PRIVATE PkgConfig::xcursor)
  if(DRM_CXX_HAS_XCURSOR_DELAY)
    target_compile_definitions(drm-cxx PRIVATE HAS_XCURSOR_DELAY)
  endif()
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

if(NOT DRM_CXX_INSTALL)
  return()
endif()

include(CMakePackageConfigHelpers)

install(FILES
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
    input/pointer.hpp
    input/event_dispatcher.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/input)

install(FILES
    display/connector_info.hpp
    display/edid.hpp
    display/hdr_metadata.hpp
    display/hotplug_monitor.hpp
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

install(FILES
    gbm/device.hpp
    gbm/surface.hpp
    gbm/buffer.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/gbm)

install(FILES sync/fence.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/sync)

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
