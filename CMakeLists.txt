# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.21)
project(drm-cxx
  VERSION 1.0.0
  DESCRIPTION "C++17 DRM/KMS library"
  # C is for the vendored X11-free .xcursor loader (third_party/xcursor-mini).
  LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "drm-cxx requires Linux (DRM/KMS/GBM).")
endif()

# Defaults switch on top-level vs subdirectory consumption.
option(DRM_CXX_VULKAN           "Build Vulkan display support" ON)
option(DRM_CXX_BUILD_EXAMPLES   "Build example programs"       ${PROJECT_IS_TOP_LEVEL})
option(DRM_CXX_BUILD_TESTS      "Build unit tests"             ${PROJECT_IS_TOP_LEVEL})
option(DRM_CXX_BUILD_BENCHMARKS "Build perf benchmarks"        ${PROJECT_IS_TOP_LEVEL})
option(DRM_CXX_INSTALL          "Generate install rules"       ${PROJECT_IS_TOP_LEVEL})

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

# Optional drm::scene::GstAppsinkSource. Needs four GStreamer packages:
# gstreamer-1.0, gstreamer-app-1.0 (appsink pull), gstreamer-allocators-1.0
# (DMABUF zero-copy path), and gstreamer-video-1.0 (per-plane offsets/strides).
set(DRM_CXX_GSTREAMER "AUTO" CACHE STRING
    "Build drm::scene::GstAppsinkSource on top of GStreamer: AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_GSTREAMER PROPERTY STRINGS AUTO ON OFF)

# Optional drm::scene EGL Streams support. Headers-only build dep
# (libEGL is dlopen'd at runtime by stream_capability.cpp /
# EglStreamSource), gated to keep Mesa-only systems from carrying the
# header dep when streams are explicitly off. AUTO probes for EGL headers
# and falls back to a no-op probe when they are absent.
set(DRM_CXX_STREAMS "AUTO" CACHE STRING
    "Build drm::scene EGL Streams support (NVIDIA proprietary / Tegra): AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_STREAMS PROPERTY STRINGS AUTO ON OFF)

# EGL render-offload + GPU composition (GlScanoutProducer / GlCompositor).
# Needs EGL headers; libEGL/libGLESv2 are dlopen'd at runtime, never linked.
# Independent of streams (streams implies egl). AUTO probes for EGL headers.
set(DRM_CXX_EGL "AUTO" CACHE STRING
    "Build drm::present/drm::scene EGL render-offload + GPU composition: AUTO, ON, or OFF")
set_property(CACHE DRM_CXX_EGL PROPERTY STRINGS AUTO ON OFF)

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

# Mirror meson's b_lto=true, but only for optimized configs (per-config LTO
# is the CMake idiom; enabling it in Debug wastes local build time).
include(CheckIPOSupported)
check_ipo_supported(RESULT _drm_cxx_ipo_ok OUTPUT _drm_cxx_ipo_msg)
if(_drm_cxx_ipo_ok)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

find_package(PkgConfig REQUIRED)

# Promote a third-party imported target so its INTERFACE_INCLUDE_DIRECTORIES
# also flow through INTERFACE_SYSTEM_INCLUDE_DIRECTORIES. Without this,
# pkg_check_modules / find_package targets whose headers live outside the
# compiler's default system search path (e.g. CI's $GITHUB_WORKSPACE/foo-install
# install prefixes for libdisplay-info / blend2d / libcamera) get -I rather
# than -isystem -- which means clang-tidy treats them as user code and fails
# on their own naming-convention or unused-includes diagnostics. Apt-installed
# /usr/include deps already get -isystem from the toolchain default; this
# helper makes the behavior uniform regardless of install prefix, matching
# meson's `include_type : 'system'` on every third-party `dependency()`.
function(_drm_cxx_promote_system_includes target)
  if(NOT TARGET ${target})
    return()
  endif()
  get_target_property(_alias ${target} ALIASED_TARGET)
  if(_alias)
    set(_real ${_alias})
  else()
    set(_real ${target})
  endif()
  get_target_property(_incs ${_real} INTERFACE_INCLUDE_DIRECTORIES)
  if(_incs)
    set_property(TARGET ${_real}
                 APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_incs})
  endif()
endfunction()

pkg_check_modules(drm            REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(gbm            REQUIRED IMPORTED_TARGET gbm)

include(CheckSymbolExists)
set(CMAKE_REQUIRED_INCLUDES ${gbm_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${gbm_LIBRARIES})
check_symbol_exists(gbm_bo_create_with_modifiers2 "gbm.h" HAVE_GBM_BO_CREATE_WITH_MODIFIERS2)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
pkg_check_modules(libinput       REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(libudev        REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(xkbcommon      REQUIRED IMPORTED_TARGET xkbcommon)
# Require >=0.2.0 for the HDR static metadata, color primaries, and
# signal-colorimetry APIs consumed by src/display/edid.cpp.
pkg_check_modules(libdisplayinfo REQUIRED IMPORTED_TARGET libdisplay-info>=0.2.0)
foreach(_t drm gbm libinput libudev xkbcommon libdisplayinfo)
  _drm_cxx_promote_system_includes(PkgConfig::${_t})
endforeach()

find_library(RT_LIBRARY NAMES rt REQUIRED)
find_library(DL_LIBRARY NAMES dl REQUIRED)

if(DRM_CXX_VULKAN)
  pkg_check_modules(vulkan REQUIRED IMPORTED_TARGET vulkan)
  _drm_cxx_promote_system_includes(PkgConfig::vulkan)
endif()

# libseat probe — optional, per DRM_CXX_SESSION. Sourced strictly from
# the system via pkg-config; not vendored.
if(NOT DRM_CXX_SESSION STREQUAL "OFF")
  if(DRM_CXX_SESSION STREQUAL "ON")
    pkg_check_modules(libseat REQUIRED IMPORTED_TARGET libseat)
  else()
    pkg_check_modules(libseat IMPORTED_TARGET libseat)
  endif()
  _drm_cxx_promote_system_includes(PkgConfig::libseat)
endif()
if(TARGET PkgConfig::libseat)
  set(DRM_CXX_HAS_LIBSEAT 1)
else()
  set(DRM_CXX_HAS_LIBSEAT 0)
endif()

# GStreamer probe for drm::scene::GstAppsinkSource. Four packages, all
# required together: appsink-1.0 for the pull API, allocators-1.0 for
# the DMABUF zero-copy path, video-1.0 for per-plane offsets/strides
# via GstVideoMeta. ON = hard-fail if any one is missing; AUTO = silent
# disable; OFF = skip entirely.
if(NOT DRM_CXX_GSTREAMER STREQUAL "OFF")
  if(DRM_CXX_GSTREAMER STREQUAL "ON")
    pkg_check_modules(gstreamer       REQUIRED IMPORTED_TARGET gstreamer-1.0)
    pkg_check_modules(gstreamer_app   REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
    pkg_check_modules(gstreamer_alloc REQUIRED IMPORTED_TARGET gstreamer-allocators-1.0)
    pkg_check_modules(gstreamer_video REQUIRED IMPORTED_TARGET gstreamer-video-1.0)
  else()
    pkg_check_modules(gstreamer       IMPORTED_TARGET gstreamer-1.0)
    pkg_check_modules(gstreamer_app   IMPORTED_TARGET gstreamer-app-1.0)
    pkg_check_modules(gstreamer_alloc IMPORTED_TARGET gstreamer-allocators-1.0)
    pkg_check_modules(gstreamer_video IMPORTED_TARGET gstreamer-video-1.0)
  endif()
  foreach(_t gstreamer gstreamer_app gstreamer_alloc gstreamer_video)
    _drm_cxx_promote_system_includes(PkgConfig::${_t})
  endforeach()
endif()
if(TARGET PkgConfig::gstreamer AND TARGET PkgConfig::gstreamer_app
    AND TARGET PkgConfig::gstreamer_alloc AND TARGET PkgConfig::gstreamer_video)
  set(DRM_CXX_HAS_GSTREAMER 1)
else()
  set(DRM_CXX_HAS_GSTREAMER 0)
  if(NOT DRM_CXX_GSTREAMER STREQUAL "OFF")
    message(STATUS
      "drm-cxx: gstreamer not found — drm::scene::GstAppsinkSource will not be built")
  endif()
endif()

# EGL Streams headers-only probe. Try egl.pc first (Khronos
# canonical), then fall back to a bare <EGL/egl.h> check on the include
# path. Result is a DRM_CXX_HAS_EGL_STREAMS gate consumed by
# src/CMakeLists.txt; libEGL itself is never linked. AUTO is silent,
# ON hard-fails when the headers are absent, OFF skips entirely.
if(NOT DRM_CXX_STREAMS STREQUAL "OFF")
  if(DRM_CXX_STREAMS STREQUAL "ON")
    pkg_check_modules(egl REQUIRED IMPORTED_TARGET egl)
  else()
    pkg_check_modules(egl IMPORTED_TARGET egl)
  endif()
  if(TARGET PkgConfig::egl)
    _drm_cxx_promote_system_includes(PkgConfig::egl)
    set(DRM_CXX_HAS_EGL_STREAMS 1)
  else()
    include(CheckIncludeFileCXX)
    check_include_file_cxx("EGL/egl.h" DRM_CXX_HAS_EGL_HEADER)
    check_include_file_cxx("EGL/eglext.h" DRM_CXX_HAS_EGLEXT_HEADER)
    if(DRM_CXX_HAS_EGL_HEADER AND DRM_CXX_HAS_EGLEXT_HEADER)
      set(DRM_CXX_HAS_EGL_STREAMS 1)
    else()
      set(DRM_CXX_HAS_EGL_STREAMS 0)
      if(DRM_CXX_STREAMS STREQUAL "ON")
        message(FATAL_ERROR
          "drm-cxx: DRM_CXX_STREAMS=ON requested but EGL headers (<EGL/egl.h>, "
          "<EGL/eglext.h>) are not on the include path. Install libegl-dev / "
          "mesa-libEGL-devel and reconfigure.")
      endif()
    endif()
  endif()
else()
  set(DRM_CXX_HAS_EGL_STREAMS 0)
endif()
if(NOT DRM_CXX_HAS_EGL_STREAMS AND NOT DRM_CXX_STREAMS STREQUAL "OFF")
  message(STATUS "drm-cxx: EGL headers not found — drm::scene EGL Streams support will be a runtime no-op")
endif()

# EGL feature (render-offload + GPU composition), independent of streams.
# Mirrors the streams probe: egl.pc first (reusing PkgConfig::egl when the
# streams probe already found it), then a bare <EGL/egl.h> fallback (eglext.h is
# not standalone-includable, so it isn't checked separately). libEGL/libGLESv2
# are never linked. streams implies egl, so DRM_CXX_HAS_EGL is the union.
if(NOT DRM_CXX_EGL STREQUAL "OFF")
  if(NOT TARGET PkgConfig::egl)
    if(DRM_CXX_EGL STREQUAL "ON")
      pkg_check_modules(egl REQUIRED IMPORTED_TARGET egl)
    else()
      pkg_check_modules(egl IMPORTED_TARGET egl)
    endif()
  endif()
  if(TARGET PkgConfig::egl)
    _drm_cxx_promote_system_includes(PkgConfig::egl)
    set(DRM_CXX_EGL_FEATURE 1)
  else()
    include(CheckIncludeFileCXX)
    check_include_file_cxx("EGL/egl.h" DRM_CXX_HAS_EGL_HEADER)
    if(DRM_CXX_HAS_EGL_HEADER)
      set(DRM_CXX_EGL_FEATURE 1)
    else()
      set(DRM_CXX_EGL_FEATURE 0)
      if(DRM_CXX_EGL STREQUAL "ON")
        message(FATAL_ERROR
          "drm-cxx: DRM_CXX_EGL=ON requested but EGL headers (<EGL/egl.h>) are "
          "not on the include path. Install libegl-dev / mesa-libEGL-devel and "
          "reconfigure.")
      endif()
    endif()
  endif()
else()
  set(DRM_CXX_EGL_FEATURE 0)
endif()
if(DRM_CXX_HAS_EGL_STREAMS OR DRM_CXX_EGL_FEATURE)
  set(DRM_CXX_HAS_EGL 1)
else()
  set(DRM_CXX_HAS_EGL 0)
endif()

# drm::cursor's .xcursor loader is vendored, X11-free, and static
# (third_party/xcursor-mini) — no system libxcursor / X11 chain. The
# feature is available unless explicitly turned off; the vendored
# XcursorImage carries the `delay` field.
if(NOT DRM_CXX_CURSOR STREQUAL "OFF")
  set(DRM_CXX_HAS_XCURSOR 1)
  set(DRM_CXX_HAS_XCURSOR_DELAY 1)
else()
  set(DRM_CXX_HAS_XCURSOR 0)
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
    _drm_cxx_promote_system_includes(blend2d::blend2d)
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

# toml++ backs drm::csd theme loading. Header-only, C++17, only pulled
# when the Blend2D gate resolves on. Prefer a system install
# (Fedora tomlplusplus-devel, Arch tomlplusplus); fall back to the
# vendored subproject the meson wrap also points at. INTERFACE target
# with SYSTEM include dirs so toml++'s own warnings stay quiet under
# -Werror clang-tidy (same treatment as tl-expected / tcb-span above).
if(DRM_CXX_HAS_BLEND2D AND NOT TARGET tomlplusplus::tomlplusplus)
  find_package(tomlplusplus CONFIG QUIET)
  if(NOT tomlplusplus_FOUND)
    _drm_cxx_fetch_header_only(tomlplusplus
      https://github.com/marzer/tomlplusplus.git v3.4.0
      include/toml++/toml.hpp)
    add_library(drm-cxx-tomlplusplus INTERFACE)
    target_include_directories(drm-cxx-tomlplusplus SYSTEM INTERFACE
      $<BUILD_INTERFACE:${_drm_cxx_tomlplusplus_dir}/include>)
    add_library(tomlplusplus::tomlplusplus ALIAS drm-cxx-tomlplusplus)
  else()
    # System config exposes the imported target but doesn't mark its
    # headers SYSTEM. Promote them so -Werror tidy passes don't see
    # toml++ diagnostics.
    get_target_property(_tomlpp_alias tomlplusplus::tomlplusplus ALIASED_TARGET)
    if(_tomlpp_alias)
      set(_tomlpp_real ${_tomlpp_alias})
    else()
      set(_tomlpp_real tomlplusplus::tomlplusplus)
    endif()
    get_target_property(_tomlpp_incs ${_tomlpp_real} INTERFACE_INCLUDE_DIRECTORIES)
    if(_tomlpp_incs)
      set_property(TARGET ${_tomlpp_real}
                   APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_tomlpp_incs})
    endif()
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

if(DRM_CXX_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
