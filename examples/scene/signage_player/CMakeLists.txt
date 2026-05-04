# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# signage_player — four-layer LayerScene demo driven by a toml playlist.
# Included from examples/CMakeLists.txt.

include(FetchContent)

# Prefer system / CMake-installed tomlplusplus; otherwise fetch upstream.
# Header-only, so no build configuration to carry through.
find_package(tomlplusplus CONFIG QUIET)
if(NOT tomlplusplus_FOUND AND NOT TARGET tomlplusplus::tomlplusplus)
  FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
    SOURCE_DIR     "${PROJECT_SOURCE_DIR}/subprojects/tomlplusplus")
  FetchContent_MakeAvailable(tomlplusplus)
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
