# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
#
# mdi_demo — multi-document desktop on top of drm::csd. Built only
# when the Blend2D gate is on (csd lives behind it).

executable('mdi_demo',
  'main.cpp',
  'shell.cpp',
  dependencies : [drm_cxx_dep],
  include_directories : include_directories('..'),
)