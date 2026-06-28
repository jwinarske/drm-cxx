# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# signage_player — four-layer LayerScene demo driven by a toml playlist.
# Included from examples/CMakeLists.txt.

# toml++ backs the playlist parser regardless of the blend2d gate. The root
# CMakeLists already exposes tomlplusplus::tomlplusplus when blend2d is on;
# otherwise resolve it here, preferring a system install and falling back to
# the vendored git submodule (third_party/tomlplusplus). Header-only — no
# fetch.
if(NOT TARGET tomlplusplus::tomlplusplus)
  find_package(tomlplusplus CONFIG QUIET)
endif()
if(NOT TARGET tomlplusplus::tomlplusplus)
  set(_tpp_inc "${PROJECT_SOURCE_DIR}/third_party/tomlplusplus/include")
  if(NOT EXISTS "${_tpp_inc}/toml++/toml.hpp")
    message(FATAL_ERROR
      "drm-cxx: third_party/tomlplusplus is missing — the git submodule is not "
      "checked out. Run: git submodule update --init --recursive")
  endif()
  add_library(drm-cxx-signage-tomlplusplus INTERFACE)
  target_include_directories(drm-cxx-signage-tomlplusplus SYSTEM INTERFACE "${_tpp_inc}")
  add_library(tomlplusplus::tomlplusplus ALIAS drm-cxx-signage-tomlplusplus)
endif()

# tomlplusplus emits -Wweak-vtables and similar noise the library core
# would reject under -Werror; mark its headers system so they don't
# affect our diagnostics. ALIAS targets refuse set_property, so resolve
# the aliased real target first (mirrors the fmt handling at the top
# level).
if(TARGET tomlplusplus::tomlplusplus)
  get_target_property(_tpp_alias tomlplusplus::tomlplusplus ALIASED_TARGET)
  if(_tpp_alias)
    set(_tpp_real ${_tpp_alias})
  else()
    set(_tpp_real tomlplusplus::tomlplusplus)
  endif()
  get_target_property(_tpp_incs ${_tpp_real} INTERFACE_INCLUDE_DIRECTORIES)
  if(_tpp_incs)
    set_property(TARGET ${_tpp_real}
                 APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_tpp_incs})
  endif()
endif()

# Playlist parser is split out so the test binary can link against it
# without rebuilding the toml++ wiring.
add_library(signage_playlist STATIC playlist.cpp)
target_link_libraries(signage_playlist PUBLIC tomlplusplus::tomlplusplus drm-cxx::drm-cxx)
target_include_directories(signage_playlist PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(signage_player main.cpp overlay_renderer.cpp)
target_link_libraries(signage_player PRIVATE
  drm-cxx::drm-cxx
  signage_playlist)
target_include_directories(signage_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Overlay text painter uses Blend2D when the project's gate is on.
# Distros (Fedora, Arch) ship blend2d headers under /usr/include
# whether or not we linked against blend2d, so a bare __has_include
# probe in overlay_renderer.cpp is unreliable — it can compile in
# the BL paths while the link line lacks blend2d, which breaks
# `cmake -DDRM_CXX_BLEND2D=OFF`. Drive the .cpp from the gate
# directly: define SIGNAGE_OVERLAY_HAS_BLEND2D iff we actually link
# blend2d here, and let the .cpp's #ifdef do the rest.
if(DRM_CXX_HAS_BLEND2D)
  target_link_libraries(signage_player PRIVATE blend2d::blend2d)
  target_compile_definitions(signage_player PRIVATE SIGNAGE_OVERLAY_HAS_BLEND2D)
endif()
