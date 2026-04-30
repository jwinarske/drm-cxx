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

test('test_buffer_mapping',
  executable('test_buffer_mapping',
    'unit/test_buffer_mapping.cpp',
    dependencies : test_deps,
  ),
)

test('test_log',
  executable('test_log',
    'unit/test_log.cpp',
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

  # LayerScene composition fallback against VKMS. Self-skips when vkms
  # is absent. Pulls capture::snapshot for pixel readback, so it's
  # gated on the same blend2d feature as test_capture_vkms.
  test('test_layer_scene_composition_vkms',
    executable('test_layer_scene_composition_vkms',
      'integration/test_layer_scene_composition_vkms.cpp',
      dependencies : test_deps,
    ),
  )
endif

# LayerScene Phase 2.2 property-minimization tests against VKMS.
# CommitReport diagnostics, not capture::snapshot — independent of the
# blend2d gate.
test('test_layer_scene_minimization_vkms',
  executable('test_layer_scene_minimization_vkms',
    'integration/test_layer_scene_minimization_vkms.cpp',
    dependencies : test_deps,
  ),
)

# LayerScene Phase 2.4 rebind tests against VKMS.
test('test_layer_scene_rebind_vkms',
  executable('test_layer_scene_rebind_vkms',
    'integration/test_layer_scene_rebind_vkms.cpp',
    dependencies : test_deps,
  ),
)

# Buffer-side coverage against the kernel's virtual GEM driver (VGEM):
# dumb::Buffer lifecycle, BufferMapping coherence, DMA-BUF prime export
# / cross-fd import. Self-skips when vgem isn't loaded, so this stays
# green on hosts that haven't `modprobe vgem`'d. Distinct from the VKMS
# tests above, which validate the *display* path; this validates the
# *buffer* path under it without modesetting in the way.
test('test_vgem_buffer',
  executable('test_vgem_buffer',
    'integration/test_vgem_buffer.cpp',
    dependencies : test_deps,
  ),
)

# drm::scene unit tests — scene lives in the library now (src/scene/),
# so test_scene just depends on test_deps like every other unit test.
test('test_scene',
  executable('test_scene',
    'unit/test_scene.cpp',
    dependencies : test_deps,
  ),
)

test('test_external_dma_buf_source',
  executable('test_external_dma_buf_source',
    'unit/test_external_dma_buf_source.cpp',
    dependencies : test_deps,
  ),
)

test('test_composite_canvas',
  executable('test_composite_canvas',
    'unit/test_composite_canvas.cpp',
    dependencies : test_deps,
  ),
)

# Connector-rank helper test. select_connector.hpp lives under
# examples/common, so include_directories points at examples/ to
# resolve "common/select_connector.hpp".
test('test_select_connector',
  executable('test_select_connector',
    'unit/test_select_connector.cpp',
    dependencies : test_deps,
    include_directories : include_directories('../examples'),
  ),
)

# signage_player playlist parser. signage_playlist_dep still lives under
# examples/, so the playlist test only exists when examples are on.
if get_option('examples')
  test('test_playlist',
    executable('test_playlist',
      'unit/test_playlist.cpp',
      dependencies : [gtest_dep, gtest_main_dep, signage_playlist_dep],
    ),
  )

  # test_patterns painters live under examples/, same arrangement as
  # the playlist parser above.
  test('test_test_patterns',
    executable('test_test_patterns',
      'unit/test_test_patterns.cpp',
      dependencies : [gtest_dep, gtest_main_dep, test_patterns_dep],
    ),
  )
endif
