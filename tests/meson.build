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

test('test_dumb_buffer',
  executable('test_dumb_buffer',
    'unit/test_dumb_buffer.cpp',
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

if has_blend2d
  test('test_capture_snapshot',
    executable('test_capture_snapshot',
      'unit/test_capture_snapshot.cpp',
      dependencies : test_deps,
    ),
  )

  # VKMS smoke test. Self-skips when the vkms module isn't loaded, so
  # this stays green on boxes / CI runners that haven't modprobed it.
  # Under `meson test` the skip is reported as PASSED (skipped).
  test('test_capture_vkms',
    executable('test_capture_vkms',
      'integration/test_capture_vkms.cpp',
      dependencies : test_deps,
    ),
  )
endif

# drm::scene tests link against common_scene_dep defined in
# examples/common/meson.build, so they only exist when examples are on.
if get_option('examples')
  test('test_scene',
    executable('test_scene',
      'unit/test_scene.cpp',
      dependencies : [test_deps, common_scene_dep],
    ),
  )

  # signage_player playlist parser. signage_playlist_dep links toml++
  # and the parser headers; this test pulls them in transitively.
  test('test_playlist',
    executable('test_playlist',
      'unit/test_playlist.cpp',
      dependencies : [gtest_dep, gtest_main_dep, signage_playlist_dep],
    ),
  )
endif
