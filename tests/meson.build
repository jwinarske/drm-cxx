# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

gtest_dep = dependency('gtest', required : true)
gtest_main_dep = dependency('gtest_main', required : true)

test_deps = [drm_cxx_dep, gtest_dep, gtest_main_dep]

test('test_format',
  executable('test_format',
    'unit/test_format.cpp',
    dependencies : test_deps,
  ),
)

test('test_property_store',
  executable('test_property_store',
    'unit/test_property_store.cpp',
    dependencies : test_deps,
  ),
)

test('test_device',
  executable('test_device',
    'unit/test_device.cpp',
    dependencies : test_deps,
  ),
)

test('test_atomic_builder',
  executable('test_atomic_builder',
    'unit/test_atomic_builder.cpp',
    dependencies : test_deps,
  ),
)

test('test_sync_fence',
  executable('test_sync_fence',
    'unit/test_sync_fence.cpp',
    dependencies : test_deps,
  ),
)

test('test_matching',
  executable('test_matching',
    'unit/test_matching.cpp',
    dependencies : test_deps,
  ),
)

test('test_layer',
  executable('test_layer',
    'unit/test_layer.cpp',
    dependencies : test_deps,
  ),
)

test('test_output',
  executable('test_output',
    'unit/test_output.cpp',
    dependencies : test_deps,
  ),
)

test('test_plane_allocator',
  executable('test_plane_allocator',
    'unit/test_plane_allocator.cpp',
    dependencies : test_deps,
  ),
)

test('test_input',
  executable('test_input',
    'unit/test_input.cpp',
    dependencies : test_deps,
  ),
)

test('test_mode',
  executable('test_mode',
    'unit/test_mode.cpp',
    dependencies : test_deps,
  ),
)

test('test_display',
  executable('test_display',
    'unit/test_display.cpp',
    dependencies : test_deps,
  ),
)

test('test_gbm',
  executable('test_gbm',
    'unit/test_gbm.cpp',
    dependencies : test_deps,
  ),
)

if has_xcursor
  test('test_cursor_theme',
    executable('test_cursor_theme',
      'unit/test_cursor_theme.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_cursor_cursor',
    executable('test_cursor_cursor',
      'unit/test_cursor_cursor.cpp',
      dependencies : test_deps,
    ),
  )
endif
