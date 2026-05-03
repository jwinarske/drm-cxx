# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# keyboard — interactive keyboard-input demo. Wires drm::input::Seat,
# drm::input::Keyboard, drm::input::KeyRepeater, and a Blend2D-backed
# text renderer onto a single LayerScene layer. Demonstrates the
# IBus / GTK Ctrl+Shift+u Unicode-codepoint entry sequence on top of
# the library's keyboard plumbing.
#
# The font is fetched at configure time so the demo isn't gated on
# whichever font the host distro happens to ship.

# Noto Sans Regular — sufficient for ASCII, the hex-entry buffer, and
# any BMP codepoint a tester might enter. Astral characters (e.g.
# U+1F600) will tofu without a color emoji font; the codepoint label
# rendered alongside the glyph is the source of truth.
#
# URL is the upstream Noto fonts canonical distribution. Pin the URL
# to a commit SHA + add EXPECTED_HASH for reproducible CI builds.
set(_noto_url
    "https://github.com/notofonts/notofonts.github.io/raw/main/fonts/NotoSans/hinted/ttf/NotoSans-Regular.ttf")
set(_noto_path "${CMAKE_CURRENT_BINARY_DIR}/NotoSans-Regular.ttf")

if(NOT EXISTS "${_noto_path}")
  message(STATUS "keyboard: fetching ${_noto_url}")
  file(DOWNLOAD
    "${_noto_url}"
    "${_noto_path}"
    STATUS _noto_status
    SHOW_PROGRESS
    TLS_VERIFY ON)
  list(GET _noto_status 0 _noto_rc)
  if(NOT _noto_rc EQUAL 0)
    file(REMOVE "${_noto_path}")
    list(GET _noto_status 1 _noto_msg)
    message(FATAL_ERROR "keyboard: font download failed: ${_noto_msg}")
  endif()
endif()

add_executable(keyboard main.cpp)
target_link_libraries(keyboard PRIVATE drm-cxx::drm-cxx blend2d::blend2d)
target_include_directories(keyboard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(keyboard PRIVATE
    KEYBOARD_DEMO_FONT_PATH="${_noto_path}")