# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

# ---------------------------------------------------------------------------
# Optional logind integration via sdbus-c++ (>= 2.0). When available, every
# example opens its DRM/input devices through logind's TakeControl/TakeDevice
# so VT switches and SIGKILL no longer leave the TTY frozen. The probe is
# AUTO by default — examples compile either way, with LogindSession becoming
# a no-op stub when sdbus-c++ is missing.
# ---------------------------------------------------------------------------
set(DRM_CXX_BUILD_EXAMPLES_LOGIND "AUTO" CACHE STRING
    "Link examples against sdbus-c++ for logind session support: AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_BUILD_EXAMPLES_LOGIND PROPERTY STRINGS AUTO ON OFF)

add_library(examples_logind STATIC logind_session.cpp)
target_link_libraries(examples_logind PUBLIC drm-cxx::drm-cxx)
target_include_directories(examples_logind PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT DRM_CXX_BUILD_EXAMPLES_LOGIND STREQUAL "OFF")
  if(DRM_CXX_BUILD_EXAMPLES_LOGIND STREQUAL "ON")
    pkg_check_modules(sdbus_cpp REQUIRED IMPORTED_TARGET sdbus-c++>=2.0)
  else()
    pkg_check_modules(sdbus_cpp IMPORTED_TARGET sdbus-c++>=2.0)
  endif()
endif()

if(TARGET PkgConfig::sdbus_cpp)
  # Treat sdbus-c++ headers as SYSTEM so their warnings don't bubble up
  # into our -Wall/-Werror build. pkg_check_modules's IMPORTED_TARGET
  # puts the includes on INTERFACE_INCLUDE_DIRECTORIES; mirror them onto
  # INTERFACE_SYSTEM_INCLUDE_DIRECTORIES so consumers get -isystem.
  get_target_property(_sdbus_incs PkgConfig::sdbus_cpp INTERFACE_INCLUDE_DIRECTORIES)
  if(_sdbus_incs)
    set_property(TARGET PkgConfig::sdbus_cpp
                 APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_sdbus_incs})
  endif()
  target_link_libraries(examples_logind PUBLIC PkgConfig::sdbus_cpp)
  target_compile_definitions(examples_logind PUBLIC DRM_CXX_EXAMPLES_HAVE_LOGIND=1)
else()
  target_compile_definitions(examples_logind PUBLIC DRM_CXX_EXAMPLES_HAVE_LOGIND=0)
  message(STATUS
    "examples: sdbus-c++ not found — LogindSession::open() will return nullopt "
    "at runtime (set DRM_CXX_BUILD_EXAMPLES_LOGIND=ON to hard-fail)")
endif()

foreach(_ex atomic_modeset hotplug_monitor mouse_cursor overlay_planes)
  add_executable(${_ex} ${_ex}/main.cpp)
  target_link_libraries(${_ex} PRIVATE drm-cxx::drm-cxx examples_logind)
  target_include_directories(${_ex} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

if(DRM_CXX_VULKAN)
  add_executable(vulkan_display vulkan_display/main.cpp)
  target_link_libraries(vulkan_display PRIVATE drm-cxx::drm-cxx examples_logind)
  target_include_directories(vulkan_display PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ThorVG Janitor example — scaffold only (see examples/thorvg_janitor/README.md).
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
    add_subdirectory(thorvg_janitor)
    target_link_libraries(thorvg_janitor PRIVATE examples_logind)
  else()
    message(STATUS
      "thorvg_janitor: thorvg not found, skipping "
      "(set DRM_CXX_BUILD_THORVG_JANITOR=ON to hard-fail)")
  endif()
endif()
