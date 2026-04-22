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

# Optional seat-session support via libseat. When enabled, drm-cxx ships
# a drm::session::Seat that muxes logind/seatd/builtin backends; when
# disabled (or libseat is missing at configure time), Seat::open() is a
# runtime no-op that returns nullopt. Auto mode probes; ON hard-fails
# if libseat is absent; OFF skips the probe entirely.
set(DRM_CXX_SESSION "AUTO" CACHE STRING
    "Build drm::session::Seat on top of libseat: AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_SESSION PROPERTY STRINGS AUTO ON OFF)

# Optional drm::cursor module (XCursor theme resolver + KMS cursor
# renderer). Needs libxcursor; when absent or DRM_CXX_CURSOR=OFF the
# headers still install but the .cpp TUs are left out of the library.
set(DRM_CXX_CURSOR "AUTO" CACHE STRING
    "Build drm::cursor on top of libxcursor: AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_CURSOR PROPERTY STRINGS AUTO ON OFF)

# Optional Blend2D-backed modules (drm::capture first, drm::csd next).
# Needs blend2d (pulled via its CMake config — no pkg-config upstream).
# Gated together because both modules share the same Blend2D dep; a user
# flipping this on gets capture today and csd later without having to
# learn a second option.
set(DRM_CXX_BLEND2D "AUTO" CACHE STRING
    "Build drm::capture (and later drm::csd) on top of Blend2D: AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_BLEND2D PROPERTY STRINGS AUTO ON OFF)

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

# libseat probe — optional, per DRM_CXX_SESSION. Sourced strictly from
# the system via pkg-config; not vendored.
if(NOT DRM_CXX_SESSION STREQUAL "OFF")
  if(DRM_CXX_SESSION STREQUAL "ON")
    pkg_check_modules(libseat REQUIRED IMPORTED_TARGET libseat)
  else()
    pkg_check_modules(libseat IMPORTED_TARGET libseat)
  endif()
endif()
if(TARGET PkgConfig::libseat)
  set(DRM_CXX_HAS_LIBSEAT 1)
else()
  set(DRM_CXX_HAS_LIBSEAT 0)
endif()

# libxcursor probe for drm::cursor.
if(NOT DRM_CXX_CURSOR STREQUAL "OFF")
  if(DRM_CXX_CURSOR STREQUAL "ON")
    pkg_check_modules(xcursor REQUIRED IMPORTED_TARGET xcursor)
  else()
    pkg_check_modules(xcursor IMPORTED_TARGET xcursor)
  endif()
endif()
if(TARGET PkgConfig::xcursor)
  set(DRM_CXX_HAS_XCURSOR 1)
  include(CheckStructHasMember)
  set(CMAKE_REQUIRED_INCLUDES ${xcursor_INCLUDE_DIRS})
  check_struct_has_member("XcursorImage" delay "X11/Xcursor/Xcursor.h"
    DRM_CXX_HAS_XCURSOR_DELAY LANGUAGE CXX)
  unset(CMAKE_REQUIRED_INCLUDES)
else()
  set(DRM_CXX_HAS_XCURSOR 0)
  message(STATUS "drm-cxx: libxcursor not found — drm::cursor will not be built")
endif()

# Blend2D probe for drm::capture (and future drm::csd). Upstream Blend2D
# ships a CMake package config but no pkg-config file, so this path uses
# find_package(CONFIG) rather than pkg_check_modules. Fedora's
# blend2d-devel, Arch's blend2d, and upstream's install target all drop
# /usr/lib*/cmake/blend2d/blend2d-config.cmake so find_package picks up
# `blend2d::blend2d` as an imported target.
if(NOT DRM_CXX_BLEND2D STREQUAL "OFF")
  if(DRM_CXX_BLEND2D STREQUAL "ON")
    find_package(blend2d CONFIG REQUIRED)
  else()
    find_package(blend2d CONFIG QUIET)
  endif()
endif()
if(TARGET blend2d::blend2d)
  # find_package succeeded, but some distributions ship the CMake
  # config in a separate package from the headers themselves — and a
  # tidy runner that can't reach the headers will fail the whole TU.
  # Verify the umbrella header is actually includable before advertising
  # the gate as ON.
  include(CheckIncludeFileCXX)
  get_target_property(_b2d_inc blend2d::blend2d INTERFACE_INCLUDE_DIRECTORIES)
  set(CMAKE_REQUIRED_INCLUDES ${_b2d_inc})
  check_include_file_cxx("blend2d/blend2d.h" DRM_CXX_BLEND2D_HEADER_REACHABLE)
  if(NOT DRM_CXX_BLEND2D_HEADER_REACHABLE)
    check_include_file_cxx("blend2d.h" DRM_CXX_BLEND2D_FLAT_HEADER_REACHABLE)
  endif()
  unset(CMAKE_REQUIRED_INCLUDES)
  if(DRM_CXX_BLEND2D_HEADER_REACHABLE OR DRM_CXX_BLEND2D_FLAT_HEADER_REACHABLE)
    set(DRM_CXX_HAS_BLEND2D 1)
  else()
    set(DRM_CXX_HAS_BLEND2D 0)
    message(STATUS
      "drm-cxx: blend2d::blend2d target found but neither <blend2d/blend2d.h> "
      "nor <blend2d.h> is reachable — treating as not installed")
  endif()
else()
  set(DRM_CXX_HAS_BLEND2D 0)
  if(NOT DRM_CXX_BLEND2D STREQUAL "OFF")
    message(STATUS "drm-cxx: blend2d not found — drm::capture will not be built")
  endif()
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

  # Promote whichever fmt target we ended up with to system-include so
  # its own -Werror-visible diagnostics (readability-named-parameter,
  # missing-std-forward in fmt/format.h) don't hit consumers running
  # clang-tidy under the project's strict config. Mirrors the libseat
  # and libxcursor treatment in src/CMakeLists.txt. Meson already does
  # this automatically for its subproject wraps, which is why CI tidy
  # is clean there; this brings the CMake build to parity.
  #
  # fmt::fmt from FetchContent is an ALIAS to the `fmt` target — we
  # resolve the alias before setting properties, since set_property
  # refuses to touch aliases directly.
  foreach(_fmt_target PkgConfig::fmt fmt::fmt)
    if(TARGET ${_fmt_target})
      get_target_property(_fmt_alias ${_fmt_target} ALIASED_TARGET)
      if(_fmt_alias)
        set(_fmt_real ${_fmt_alias})
      else()
        set(_fmt_real ${_fmt_target})
      endif()
      get_target_property(_fmt_incs ${_fmt_real} INTERFACE_INCLUDE_DIRECTORIES)
      if(_fmt_incs)
        set_property(TARGET ${_fmt_real}
                     APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_fmt_incs})
      endif()
    endif()
  endforeach()
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
