# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

# Benchmarks share the example helpers (open_output, format_probe) —
# add the examples/ tree to the include path so `#include
# "common/foo.hpp"` resolves the same way as in the example tree.
benchmarks_inc = include_directories('..' / 'examples')

executable('plane_stress',
  'plane_stress/main.cpp',
  include_directories : benchmarks_inc,
  dependencies : [drm_cxx_dep],
)

executable('allocator_torture',
  'allocator_torture/main.cpp',
  include_directories : benchmarks_inc,
  dependencies : [drm_cxx_dep],
)