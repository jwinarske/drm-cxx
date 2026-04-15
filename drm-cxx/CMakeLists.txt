# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

set(DRM_CXX_SOURCES
  core/device.cpp
  core/resources.cpp
  core/property_store.cpp
  core/format.cpp
  modeset/atomic.cpp
  modeset/mode.cpp
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
  gbm/device.cpp
  gbm/surface.cpp
  gbm/buffer.cpp
  sync/fence.cpp)

if(DRM_CXX_VULKAN)
  list(APPEND DRM_CXX_SOURCES
    vulkan/display.cpp
    vulkan/drm_surface.cpp)
endif()

include(GNUInstallDirs)

add_library(drm-cxx ${DRM_CXX_SOURCES})
add_library(drm-cxx::drm-cxx ALIAS drm-cxx)

set_target_properties(drm-cxx PROPERTIES
  VERSION     ${PROJECT_VERSION}
  SOVERSION   ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME drm-cxx)

# Two public include roots:
#   - drm-cxx/ so internal cross-subdir includes like "core/foo.hpp" work
#     (matches meson's include_directories('.'))
#   - the project root so public headers can use <drm-cxx/detail/*.hpp>,
#     which is also the form consumers get through pkg-config's
#     `-I${includedir}` and through the installed include layout.
target_include_directories(drm-cxx
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/drm-cxx>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx>
  PRIVATE
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>)

target_compile_features(drm-cxx PUBLIC cxx_std_23)

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

if(DRM_CXX_VULKAN)
  target_link_libraries(drm-cxx PUBLIC PkgConfig::vulkan)
endif()

if(NOT DRM_CXX_HAS_STD_PRINT)
  target_link_libraries(drm-cxx PUBLIC PkgConfig::fmt)
endif()

# TODO(Phase D): when C++17 migration lands, wire tl-expected and tcb-span
# here via FetchContent with find_package(...CONFIG QUIET) fallback. The
# detail/ adapter headers currently alias std::expected / std::span under
# C++23, so both are unused at compile time today.

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
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/drm-cxx/display)

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
else()
  set(PC_FMT_REQUIRES " fmt")
endif()

configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/drm-cxx.pc.in"
  "${PROJECT_BINARY_DIR}/drm-cxx.pc"
  @ONLY)

install(FILES "${PROJECT_BINARY_DIR}/drm-cxx.pc"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
