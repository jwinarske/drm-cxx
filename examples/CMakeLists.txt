# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

foreach(_ex atomic_modeset hotplug_monitor mouse_cursor overlay_planes)
  add_executable(${_ex} ${_ex}/main.cpp)
  target_link_libraries(${_ex} PRIVATE drm-cxx::drm-cxx)
  target_include_directories(${_ex} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# XCursor loader + smoke test for the mouse_cursor example.
# mouse_cursor itself picks up the dep in a later step of xcursor_plan.md.
pkg_check_modules(xcursor REQUIRED IMPORTED_TARGET xcursor)

include(CheckStructHasMember)
set(CMAKE_REQUIRED_INCLUDES ${xcursor_INCLUDE_DIRS})
check_struct_has_member("XcursorImage" delay "X11/Xcursor/Xcursor.h"
                        HAS_XCURSOR_DELAY LANGUAGE C)
unset(CMAKE_REQUIRED_INCLUDES)

add_executable(xcursor_smoke
  mouse_cursor/xcursor_smoke.cpp
  mouse_cursor/xcursor_loader.cpp)
target_link_libraries(xcursor_smoke PRIVATE PkgConfig::xcursor)
if(HAS_XCURSOR_DELAY)
  target_compile_definitions(xcursor_smoke PRIVATE HAS_XCURSOR_DELAY)
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