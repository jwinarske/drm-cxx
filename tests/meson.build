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

test('test_key_repeater',
  executable('test_key_repeater',
    'unit/test_key_repeater.cpp',
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

test('test_color_pipeline_curves',
  executable('test_color_pipeline_curves',
    'unit/test_color_pipeline_curves.cpp',
    dependencies : test_deps,
  ),
)

test('test_connector_capabilities',
  executable('test_connector_capabilities',
    'unit/test_connector_capabilities.cpp',
    dependencies : test_deps,
  ),
)

test('test_hdr_metadata',
  executable('test_hdr_metadata',
    'unit/test_hdr_metadata.cpp',
    dependencies : test_deps,
  ),
)

test('test_hdr_metadata_cache',
  executable('test_hdr_metadata_cache',
    'unit/test_hdr_metadata_cache.cpp',
    dependencies : test_deps,
  ),
)

test('test_output_signaling',
  executable('test_output_signaling',
    'unit/test_output_signaling.cpp',
    dependencies : test_deps,
  ),
)

test('test_tone_mapper',
  executable('test_tone_mapper',
    'unit/test_tone_mapper.cpp',
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

  test('test_csd_theme',
    executable('test_csd_theme',
      'unit/test_csd_theme.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_csd_surface',
    executable('test_csd_surface',
      'unit/test_csd_surface.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_csd_shadow_cache',
    executable('test_csd_shadow_cache',
      'unit/test_csd_shadow_cache.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_csd_renderer',
    executable('test_csd_renderer',
      'unit/test_csd_renderer.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_csd_animator',
    executable('test_csd_animator',
      'unit/test_csd_animator.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_csd_overlay_reservation',
    executable('test_csd_overlay_reservation',
      'unit/test_csd_overlay_reservation.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_csd_presenter_plane',
    executable('test_csd_presenter_plane',
      'unit/test_csd_presenter_plane.cpp',
      dependencies : test_deps,
    ),
  )

  # VKMS smoke test. Self-skips when the vkms module isn't loaded, so
  # this stays green on boxes / CI runners that haven't modprobed it.
  # Under `meson test` the skip is reported as PASSED (skipped).
  #
  # `is_parallel : false` on every *_vkms test below: vkms is a
  # single-device DRM driver and only one process can hold master at
  # a time. Letting these run concurrently produces EACCES on every
  # atomic commit but the master-holding test, which manifests as
  # flaky 7-of-14 failures under `meson test`. Serializing them with
  # each other (non-vkms tests still run in parallel) is the cheapest
  # correctness fix.
  test('test_capture_vkms',
    executable('test_capture_vkms',
      'integration/test_capture_vkms.cpp',
      dependencies : test_deps,
    ),
    is_parallel : false,
  )

  # LayerScene composition fallback against VKMS. Self-skips when vkms
  # is absent. Pulls capture::snapshot for pixel readback, so it's
  # gated on the same blend2d feature as test_capture_vkms.
  test('test_layer_scene_composition_vkms',
    executable('test_layer_scene_composition_vkms',
      'integration/test_layer_scene_composition_vkms.cpp',
      dependencies : test_deps,
    ),
    is_parallel : false,
  )
endif

# LayerScene property-minimization tests against VKMS.
# CommitReport diagnostics, not capture::snapshot — independent of the
# blend2d gate.
test('test_layer_scene_minimization_vkms',
  executable('test_layer_scene_minimization_vkms',
    'integration/test_layer_scene_minimization_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

# Hardware-gated EGL Streams integration. All TESTs self-skip via
# GTEST_SKIP when probe_stream_capability returns Unsupported, so
# CI hosts on Mesa-only stacks see the binary run-and-skip cleanly.
test('test_egl_streams_hw',
  executable('test_egl_streams_hw',
    'integration/test_egl_streams_hw.cpp',
    dependencies : test_deps,
  ),
)

# CRTC color-pipeline integration against vkms.
test('test_crtc_color_pipeline_vkms',
  executable('test_crtc_color_pipeline_vkms',
    'integration/test_crtc_color_pipeline_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

# Buffer::create_planar(P010) round-trip against vkms.
# Self-skips when vkms isn't loaded. No capture::snapshot dependency
# (we verify the commit chain, not pixels).
test('test_dumb_buffer_p010_vkms',
  executable('test_dumb_buffer_p010_vkms',
    'integration/test_dumb_buffer_p010_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

# LayerScene rebind tests against VKMS.
test('test_layer_scene_rebind_vkms',
  executable('test_layer_scene_rebind_vkms',
    'integration/test_layer_scene_rebind_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

# LayerScene deferred-release contract tests against VKMS. Pins the
# release timing for LayerBufferSource so the tearing bug fixed by
# the prev/prev_prev acquisition ring doesn't regress.
test('test_layer_scene_release_vkms',
  executable('test_layer_scene_release_vkms',
    'integration/test_layer_scene_release_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

# SceneSet combined-commit + mirrored-layer tests against a vkms
# instance with >=2 connected outputs. Skips when no qualifying
# vkms is present (see the test source header for the configfs
# provisioning recipe).
test('test_scene_set_vkms',
  executable('test_scene_set_vkms',
    'integration/test_scene_set_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
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

test('test_scene_set',
  executable('test_scene_set',
    'unit/test_scene_set.cpp',
    dependencies : test_deps,
  ),
)

test('test_stream_capability',
  executable('test_stream_capability',
    'unit/test_stream_capability.cpp',
    dependencies : test_deps,
  ),
)

test('test_egl_stream_builder',
  executable('test_egl_stream_builder',
    'unit/test_egl_stream_builder.cpp',
    dependencies : test_deps,
  ),
)

test('test_egl_stream_source',
  executable('test_egl_stream_source',
    'unit/test_egl_stream_source.cpp',
    dependencies : test_deps,
  ),
)

test('test_external_dma_buf_source',
  executable('test_external_dma_buf_source',
    'unit/test_external_dma_buf_source.cpp',
    dependencies : test_deps,
  ),
)

# GstAppsinkSource validation test — runs in both build modes; the
# real build adds gstreamer + gstreamer-app at link time + the
# DRM_CXX_HAS_GSTREAMER=1 define so the test fixture compiles.
gst_test_deps = test_deps
gst_test_args = ['-DDRM_CXX_HAS_GSTREAMER=0']
if has_gstreamer
  gst_test_deps = test_deps + [gst_dep, gst_app_dep]
  gst_test_args = ['-DDRM_CXX_HAS_GSTREAMER=1']
endif
test('test_gst_appsink_source',
  executable('test_gst_appsink_source',
    'unit/test_gst_appsink_source.cpp',
    dependencies : gst_test_deps,
    cpp_args : gst_test_args,
  ),
)

# Vkms integration test for GstAppsinkSource — only built when the
# GStreamer gate is on. Self-skips at runtime when vkms isn't loaded.
if has_gstreamer
  test('test_gst_appsink_source_vkms',
    executable('test_gst_appsink_source_vkms',
      'integration/test_gst_appsink_source_vkms.cpp',
      dependencies : test_deps + [gst_dep, gst_app_dep],
    ),
    is_parallel : false,
  )
endif

test('test_v4l2_decoder_source',
  executable('test_v4l2_decoder_source',
    'unit/test_v4l2_decoder_source.cpp',
    dependencies : test_deps,
  ),
)

test('test_v4l2_camera_source',
  executable('test_v4l2_camera_source',
    'unit/test_v4l2_camera_source.cpp',
    dependencies : test_deps,
  ),
)

test('test_gbm_surface_source',
  executable('test_gbm_surface_source',
    'unit/test_gbm_surface_source.cpp',
    dependencies : test_deps,
  ),
)

# GbmSurfaceSource integration test against vkms. Self-skips when
# vkms isn't loaded.
test('test_gbm_surface_source_vkms',
  executable('test_gbm_surface_source_vkms',
    'integration/test_gbm_surface_source_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

# V4l2CameraSource integration test against any /dev/video* + vkms.
# Self-skips at runtime when no usable CAPTURE device is present or
# vkms isn't loaded.
test('test_v4l2_camera_source_vivid',
  executable('test_v4l2_camera_source_vivid',
    'integration/test_v4l2_camera_source_vivid.cpp',
    dependencies : test_deps,
  ),
)

# V4l2DecoderSource integration test against vicodec + vkms. Self-skips
# at runtime when either kernel module isn't loaded.
test('test_v4l2_decoder_source_vicodec',
  executable('test_v4l2_decoder_source_vicodec',
    'integration/test_v4l2_decoder_source_vicodec.cpp',
    dependencies : test_deps,
  ),
)

test('test_page_flip',
  executable('test_page_flip',
    'unit/test_page_flip.cpp',
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
