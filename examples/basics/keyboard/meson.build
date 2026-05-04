# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# keyboard — interactive keyboard-input demo. Mirrors the CMake build:
# fetch Noto Sans Regular at configure time, compile main.cpp against
# drm-cxx + Blend2D, hand the font path through as a compile define.

noto_url = 'https://github.com/notofonts/notofonts.github.io/raw/main/fonts/NotoSans/hinted/ttf/NotoSans-Regular.ttf'
noto_filename = 'NotoSans-Regular.ttf'

# Meson has no first-class file(DOWNLOAD) equivalent that runs at
# configure time; use a custom_target driven by curl so the file lands
# in the build dir before compilation. `--fail` makes curl exit non-zero
# on HTTP errors; `--retry 3` masks transient 5xx responses.
noto_ttf = custom_target('keyboard_noto_sans',
  output : noto_filename,
  command : [find_program('curl'),
             '--silent', '--fail', '--location',
             '--retry', '3', '--retry-delay', '2',
             '--output', '@OUTPUT@',
             noto_url],
  build_by_default : true,
)

executable('keyboard',
  'main.cpp',
  noto_ttf,                          # ensures download runs before compile
  dependencies : [drm_cxx_dep, blend2d_dep],
  cpp_args : ['-DKEYBOARD_DEMO_FONT_PATH="' + meson.current_build_dir() / noto_filename + '"'],
  include_directories : include_directories('..'),
)