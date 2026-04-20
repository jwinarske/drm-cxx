# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# Included from examples/meson.build only when thorvg_dep is found. The
# parent build file handles the auto/enabled/disabled feature logic; by
# the time we get here, thorvg is usable.

executable('thorvg_janitor',
  'main.cpp',
  dependencies : [drm_cxx_dep, thorvg_dep],
)
