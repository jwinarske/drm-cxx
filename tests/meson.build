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

test('test_format_mod',
  executable('test_format_mod',
    'unit/test_format_mod.cpp',
    dependencies : test_deps,
  ),
)

test('test_v4l2_plane_layout',
  executable('test_v4l2_plane_layout',
    'unit/test_v4l2_plane_layout.cpp',
    dependencies : test_deps,
  ),
)

test('test_negotiate',
  executable('test_negotiate',
    'unit/test_negotiate.cpp',
    dependencies : test_deps,
  ),
)

test('test_buffer_ring',
  executable('test_buffer_ring',
    'unit/test_buffer_ring.cpp',
    dependencies : test_deps,
  ),
)

test('test_frame_economy',
  executable('test_frame_economy',
    'unit/test_frame_economy.cpp',
    dependencies : test_deps,
  ),
)

# Integration: needs a dumb-capable DRM card (any driver; skips if none).
test('test_dumb_ring_source',
  executable('test_dumb_ring_source',
    'integration/test_dumb_ring_source.cpp',
    dependencies : test_deps,
  ),
)

# GPU compositor pixel-correctness check. Needs a GL-capable KMS card; skips on
# vkms/CI and GPU-less builds (GTEST_SKIP). HW check on StarFive / Steam Deck.
test('test_gl_compositor_readback',
  executable('test_gl_compositor_readback',
    'integration/test_gl_compositor_readback.cpp',
    dependencies : test_deps,
  ),
)

test('test_scanout_target',
  executable('test_scanout_target',
    'unit/test_scanout_target.cpp',
    dependencies : test_deps,
  ),
)

test('test_driver_profile',
  executable('test_driver_profile',
    'unit/test_driver_profile.cpp',
    dependencies : test_deps,
  ),
)

test('test_mode_list',
  executable('test_mode_list',
    'unit/test_mode_list.cpp',
    dependencies : test_deps,
  ),
)

test('test_dma_buf_source_cache',
  executable('test_dma_buf_source_cache',
    'unit/test_dma_buf_source_cache.cpp',
    dependencies : test_deps,
  ),
)

test('test_dumb_scanout_sink',
  executable('test_dumb_scanout_sink',
    'unit/test_dumb_scanout_sink.cpp',
    dependencies : test_deps,
  ),
)

# Integration: needs a dumb-capable modeset card (vkms/amdgpu/rockchip; skips
# if none). Serial — shares the DRM device with the other card-backed tests.
test('test_dma_buf_source_cache_vkms',
  executable('test_dma_buf_source_cache_vkms',
    'integration/test_dma_buf_source_cache_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

test('test_external_dma_buf_ring_vkms',
  executable('test_external_dma_buf_ring_vkms',
    'integration/test_external_dma_buf_ring_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

test('test_external_dma_buf_ring_scene_vkms',
  executable('test_external_dma_buf_ring_scene_vkms',
    'integration/test_external_dma_buf_ring_scene_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

# Integration: DumbScanoutSink present -> flip -> page-flip event on vkms. Serial
# (modesets); self-skips when vkms is absent.
test('test_dumb_scanout_sink_vkms',
  executable('test_dumb_scanout_sink_vkms',
    'integration/test_dumb_scanout_sink_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

test('test_acquire_fence',
  executable('test_acquire_fence',
    'unit/test_acquire_fence.cpp',
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

  test('test_cursor_source',
    executable('test_cursor_source',
      'integration/test_cursor_source.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_cursor_renderer_legacy',
    executable('test_cursor_renderer_legacy',
      'integration/test_cursor_renderer_legacy.cpp',
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

  test('test_csd_presenter_composite',
    executable('test_csd_presenter_composite',
      'unit/test_csd_presenter_composite.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_csd_presenter_fb',
    executable('test_csd_presenter_fb',
      'unit/test_csd_presenter_fb.cpp',
      dependencies : test_deps,
    ),
  )

  test('test_csd_probe_presenter',
    executable('test_csd_probe_presenter',
      'unit/test_csd_probe_presenter.cpp',
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

test('test_layer_scene_content_type_vkms',
  executable('test_layer_scene_content_type_vkms',
    'integration/test_layer_scene_content_type_vkms.cpp',
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

test('test_external_dma_buf_ring',
  executable('test_external_dma_buf_ring',
    'unit/test_external_dma_buf_ring.cpp',
    dependencies : test_deps,
  ),
)

test('test_commit_report',
  executable('test_commit_report',
    'unit/test_commit_report.cpp',
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

test('test_scanout_backend_vkms',
  executable('test_scanout_backend_vkms',
    'integration/test_scanout_backend_vkms.cpp',
    dependencies : test_deps,
  ),
  is_parallel : false,
)

# Explicit-sync IN_FENCE_FD validation against a real GPU + atomic KMS driver.
# A GL producer's EGL native fence becomes the plane's IN_FENCE_FD; self-skips
# where no real GPU fence is produced (llvmpipe) or no KMS card. Gated on egl +
# glesv2 (the library never links them itself); the source also guards on
# DRM_CXX_HAS_EGL. Not parallel: it modesets.
fence_egl_dep = dependency('egl', required : false)
fence_glesv2_dep = dependency('glesv2', required : false)
if fence_egl_dep.found() and fence_glesv2_dep.found()
  test('test_explicit_sync_fence_gl',
    executable('test_explicit_sync_fence_gl',
      'integration/test_explicit_sync_fence_gl.cpp',
      dependencies : test_deps + [fence_egl_dep, fence_glesv2_dep],
    ),
    is_parallel : false,
  )
endif

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

# Composition-canvas format negotiation (single-plane fallback core) — device-free.
test('test_canvas_format',
  executable('test_canvas_format',
    'unit/test_canvas_format.cpp',
    dependencies : test_deps,
  ),
)

# GlCompositor pure quad/texcoord math — no GL context, builds regardless of EGL.
test('test_gl_compositor_math',
  executable('test_gl_compositor_math',
    'unit/test_gl_compositor_math.cpp',
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
