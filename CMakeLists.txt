# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.21)
project(drm-cxx
  VERSION 1.0.0
  DESCRIPTION "C++23 DRM/KMS library"
  LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "drm-cxx requires Linux (DRM/KMS/GBM).")
endif()

# Defaults switch on top-level vs subdirectory consumption.
option(DRM_CXX_VULKAN         "Build Vulkan display support" ON)
option(DRM_CXX_BUILD_EXAMPLES "Build example programs"       ${PROJECT_IS_TOP_LEVEL})
option(DRM_CXX_BUILD_TESTS    "Build unit tests"             ${PROJECT_IS_TOP_LEVEL})
option(DRM_CXX_INSTALL        "Generate install rules"       ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match meson's default (shared library on Linux) for standalone builds.
# Subdir consumers can override BUILD_SHARED_LIBS before add_subdirectory.
if(PROJECT_IS_TOP_LEVEL AND NOT DEFINED BUILD_SHARED_LIBS)
  set(BUILD_SHARED_LIBS ON)
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# Mirror meson's b_lto=true, but only for optimised configs (per-config LTO
# is the CMake idiom; enabling it in Debug wastes local build time).
include(CheckIPOSupported)
check_ipo_supported(RESULT _drm_cxx_ipo_ok OUTPUT _drm_cxx_ipo_msg)
if(_drm_cxx_ipo_ok)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(drm            REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(gbm            REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(libinput       REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(libudev        REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(xkbcommon      REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(libdisplayinfo REQUIRED IMPORTED_TARGET libdisplay-info)

find_library(RT_LIBRARY NAMES rt REQUIRED)
find_library(DL_LIBRARY NAMES dl REQUIRED)

if(DRM_CXX_VULKAN)
  pkg_check_modules(vulkan REQUIRED IMPORTED_TARGET vulkan)
endif()

# {fmt} backs the drm-cxx/detail/format.hpp adapter when the toolchain lacks
# std::print / std::format. Probe the compiler with the currently selected
# C++ standard; on failure, pull fmt via pkg-config. Under C++23 the adapter
# aliases std::* directly and fmt is unused.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -std=c++${CMAKE_CXX_STANDARD}")
check_cxx_source_compiles([=[
  #include <print>
  int main() { std::println("{}", 1); return 0; }
]=] DRM_CXX_HAS_STD_PRINT)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT DRM_CXX_HAS_STD_PRINT)
  pkg_check_modules(fmt REQUIRED IMPORTED_TARGET fmt)
endif()

if(DRM_CXX_VULKAN)
  set(HAVE_VULKAN 1)
else()
  set(HAVE_VULKAN 0)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/config.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/drm-cxx/config.h"
  @ONLY)

add_subdirectory(drm-cxx)

if(DRM_CXX_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(DRM_CXX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
