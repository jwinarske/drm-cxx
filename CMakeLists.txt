# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.21)
project(drm-cxx
  VERSION 1.0.0
  DESCRIPTION "C++17 DRM/KMS library"
  LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "drm-cxx requires Linux (DRM/KMS/GBM).")
endif()

# Defaults switch on top-level vs subdirectory consumption.
option(DRM_CXX_VULKAN         "Build Vulkan display support" ON)
option(DRM_CXX_BUILD_EXAMPLES "Build example programs"       ${PROJECT_IS_TOP_LEVEL})
option(DRM_CXX_BUILD_TESTS    "Build unit tests"             ${PROJECT_IS_TOP_LEVEL})
option(DRM_CXX_INSTALL        "Generate install rules"       ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 17)
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
# C++ standard; on failure, pull fmt via pkg-config. Under C++17 the probe
# fails and {fmt} is used; under C++23 the adapter aliases std::* directly
# and fmt is unused.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -std=c++${CMAKE_CXX_STANDARD}")
check_cxx_source_compiles([=[
  #include <print>
  int main() { std::println("{}", 1); return 0; }
]=] DRM_CXX_HAS_STD_PRINT)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT DRM_CXX_HAS_STD_PRINT)
  pkg_check_modules(fmt IMPORTED_TARGET fmt)
  if(NOT fmt_FOUND)
    find_package(fmt CONFIG QUIET)
  endif()
  if(NOT fmt_FOUND AND NOT TARGET fmt::fmt)
    include(FetchContent)
    set(FMT_TEST  OFF CACHE BOOL "" FORCE)
    set(FMT_DOC   OFF CACHE BOOL "" FORCE)
    set(FMT_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(fmt
      GIT_REPOSITORY https://github.com/fmtlib/fmt.git
      GIT_TAG        11.0.2
      GIT_SHALLOW    TRUE
      SOURCE_DIR     "${PROJECT_SOURCE_DIR}/subprojects/fmt")
    FetchContent_MakeAvailable(fmt)
  endif()
endif()

# C++17 polyfills for std::expected (C++23) and std::span (C++20). The
# drm-cxx/detail/{expected,span}.hpp adapter headers alias the std::*
# versions when the compiler provides them; otherwise they fall back to
# tl::expected and tcb::span, pulled here via find_package with a
# FetchContent fallback (mirrors the meson subproject wraps).
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -std=c++${CMAKE_CXX_STANDARD}")
check_cxx_source_compiles([=[
  #include <expected>
  int main() { std::expected<int,int> e{1}; (void)e; return 0; }
]=] DRM_CXX_HAS_STD_EXPECTED)
check_cxx_source_compiles([=[
  #include <span>
  int main() { int a[3]{}; std::span<int> s{a}; return s.empty(); }
]=] DRM_CXX_HAS_STD_SPAN)
unset(CMAKE_REQUIRED_FLAGS)

# Both polyfill libs are header-only; prefer a system-installed target,
# otherwise download their source tree (reusing subprojects/ if already
# populated by `meson subprojects download`) and wrap the include dir in
# a local INTERFACE target. We deliberately avoid add_subdirectory on the
# upstream CMakeLists — their install/test rules would leak into this
# project's export set.
function(_drm_cxx_fetch_header_only name url tag relpath)
  set(_dir "${PROJECT_SOURCE_DIR}/subprojects/${name}")
  if(NOT EXISTS "${_dir}/${relpath}")
    message(STATUS "drm-cxx: cloning ${name} (${tag}) into ${_dir}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_SOURCE_DIR}/subprojects"
      RESULT_VARIABLE _rc)
    execute_process(
      COMMAND git clone --depth=1 --branch ${tag} ${url} "${_dir}"
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "drm-cxx: failed to clone ${name} from ${url}")
    endif()
  endif()
  set(_drm_cxx_${name}_dir "${_dir}" PARENT_SCOPE)
endfunction()

if(NOT DRM_CXX_HAS_STD_EXPECTED AND NOT TARGET tl::expected)
  find_package(tl-expected CONFIG QUIET)
  if(NOT tl-expected_FOUND)
    _drm_cxx_fetch_header_only(tl-expected
      https://github.com/TartanLlama/expected.git v1.1.0
      include/tl/expected.hpp)
    add_library(drm-cxx-tl-expected INTERFACE)
    target_include_directories(drm-cxx-tl-expected SYSTEM INTERFACE
      $<BUILD_INTERFACE:${_drm_cxx_tl-expected_dir}/include>)
    add_library(tl::expected ALIAS drm-cxx-tl-expected)
  endif()
endif()

if(NOT DRM_CXX_HAS_STD_SPAN AND NOT TARGET tcb::span)
  find_package(tcb-span CONFIG QUIET)
  if(NOT tcb-span_FOUND)
    # tcb/span uses a commit rev; git clone --branch accepts tags but not
    # bare SHAs, so do a full clone when this path is taken at all.
    set(_tcb_dir "${PROJECT_SOURCE_DIR}/subprojects/tcb-span")
    if(NOT EXISTS "${_tcb_dir}/include/tcb/span.hpp")
      execute_process(
        COMMAND git clone https://github.com/tcbrindle/span.git "${_tcb_dir}"
        RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "drm-cxx: failed to clone tcb-span")
      endif()
      execute_process(
        COMMAND git -C "${_tcb_dir}" checkout --detach
                836dc6a0efd9849cb194e88e4aa2387436bb079b)
    endif()
    add_library(drm-cxx-tcb-span INTERFACE)
    target_include_directories(drm-cxx-tcb-span SYSTEM INTERFACE
      $<BUILD_INTERFACE:${_tcb_dir}/include>)
    add_library(tcb::span ALIAS drm-cxx-tcb-span)
  endif()
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

# Public headers live in src/ but must be #include'd as <drm-cxx/...>, both
# in this build and after install. Expose src/ under a drm-cxx/ prefix via a
# build-tree symlink — Linux-only project, so the symlink is safe.
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/include")
if(NOT EXISTS "${PROJECT_BINARY_DIR}/include/drm-cxx")
  file(CREATE_LINK
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/include/drm-cxx"
    SYMBOLIC)
endif()

add_subdirectory(src)

if(DRM_CXX_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(DRM_CXX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
