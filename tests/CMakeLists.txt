# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT

# Prefer a consumer-provided gtest; otherwise fetch it.
find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

set(DRM_CXX_TESTS
  test_format
  test_property_store
  test_device
  test_atomic_builder
  test_sync_fence
  test_matching
  test_layer
  test_output
  test_plane_allocator
  test_input
  test_key_repeater
  test_mode
  test_display
  test_gbm
  test_dumb_buffer
  test_buffer_mapping
  test_log)

if(DRM_CXX_HAS_XCURSOR)
  list(APPEND DRM_CXX_TESTS
    test_cursor_theme
    test_cursor_cursor)
endif()

if(DRM_CXX_HAS_BLEND2D)
  list(APPEND DRM_CXX_TESTS
    test_capture_snapshot
    test_csd_theme
    test_csd_surface
    test_csd_shadow_cache
    test_csd_renderer
    test_csd_animator
    test_csd_overlay_reservation
    test_csd_presenter_plane)
endif()

foreach(_t ${DRM_CXX_TESTS})
  add_executable(${_t} unit/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE
    drm-cxx::drm-cxx
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(${_t})
endforeach()

# VKMS integration test — self-skips when vkms is absent. Kept out of
# the loop above because its source lives under tests/integration/
# rather than tests/unit/.
if(DRM_CXX_HAS_BLEND2D)
  add_executable(test_capture_vkms integration/test_capture_vkms.cpp)
  target_link_libraries(test_capture_vkms PRIVATE
    drm-cxx::drm-cxx
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(test_capture_vkms)

  # LayerScene composition fallback against VKMS — pulls capture
  # snapshot for pixel readback, so it's gated on Blend2D for the same
  # reason test_capture_vkms is.
  add_executable(test_layer_scene_composition_vkms
    integration/test_layer_scene_composition_vkms.cpp)
  target_link_libraries(test_layer_scene_composition_vkms PRIVATE
    drm-cxx::drm-cxx
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(test_layer_scene_composition_vkms)
endif()

# LayerScene Phase 2.2 property-minimization tests against VKMS. No
# Blend2D dependency — diagnostics readback is via CommitReport, not
# capture::snapshot — so this stays out of the DRM_CXX_HAS_BLEND2D
# block above.
add_executable(test_layer_scene_minimization_vkms
  integration/test_layer_scene_minimization_vkms.cpp)
target_link_libraries(test_layer_scene_minimization_vkms PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_layer_scene_minimization_vkms)

# LayerScene Phase 2.4 rebind tests against VKMS. Same rationale as
# the minimization suite — pure scene API, no capture dependency.
add_executable(test_layer_scene_rebind_vkms
  integration/test_layer_scene_rebind_vkms.cpp)
target_link_libraries(test_layer_scene_rebind_vkms PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_layer_scene_rebind_vkms)

# Buffer-side coverage against the kernel's virtual GEM driver (VGEM).
# Self-skips when vgem isn't loaded, so this stays green on hosts that
# haven't `modprobe vgem`'d. Validates the *buffer* path (dumb::Buffer
# lifecycle, BufferMapping coherence, DMA-BUF prime export / cross-fd
# import) without modesetting in the way — VKMS covers the display path.
add_executable(test_vgem_buffer
  integration/test_vgem_buffer.cpp)
target_link_libraries(test_vgem_buffer PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_vgem_buffer)

# drm::scene unit tests — scene lives in the library now (src/scene/),
# so the test binary just links drm-cxx::drm-cxx like every other unit test.
add_executable(test_scene unit/test_scene.cpp)
target_link_libraries(test_scene PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_scene)

add_executable(test_composite_canvas unit/test_composite_canvas.cpp)
target_link_libraries(test_composite_canvas PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_composite_canvas)

add_executable(test_external_dma_buf_source unit/test_external_dma_buf_source.cpp)
target_link_libraries(test_external_dma_buf_source PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_external_dma_buf_source)

# GstAppsinkSource validation test — exercises the same argument
# validation in both build modes. Real build also needs gstreamer +
# gstreamer-app at link time so the fixture can construct an appsink
# element; stub build runs without any GStreamer libs.
add_executable(test_gst_appsink_source unit/test_gst_appsink_source.cpp)
target_link_libraries(test_gst_appsink_source PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
if(DRM_CXX_HAS_GSTREAMER)
  target_link_libraries(test_gst_appsink_source PRIVATE
    PkgConfig::gstreamer
    PkgConfig::gstreamer_app)
  target_compile_definitions(test_gst_appsink_source PRIVATE DRM_CXX_HAS_GSTREAMER=1)
else()
  target_compile_definitions(test_gst_appsink_source PRIVATE DRM_CXX_HAS_GSTREAMER=0)
endif()
gtest_discover_tests(test_gst_appsink_source)

# GstAppsinkSource integration test against vkms — only built when
# the GStreamer gate is on. Self-skips at runtime when vkms isn't
# loaded so this stays green on hosts that haven't `modprobe vkms`'d.
if(DRM_CXX_HAS_GSTREAMER)
  add_executable(test_gst_appsink_source_vkms
    integration/test_gst_appsink_source_vkms.cpp)
  target_link_libraries(test_gst_appsink_source_vkms PRIVATE
    drm-cxx::drm-cxx
    PkgConfig::gstreamer
    PkgConfig::gstreamer_app
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(test_gst_appsink_source_vkms)
endif()

add_executable(test_v4l2_decoder_source unit/test_v4l2_decoder_source.cpp)
target_link_libraries(test_v4l2_decoder_source PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_v4l2_decoder_source)

# V4l2DecoderSource integration test against vicodec + vkms. Self-skips
# at runtime when either kernel module isn't loaded.
add_executable(test_v4l2_decoder_source_vicodec
  integration/test_v4l2_decoder_source_vicodec.cpp)
target_link_libraries(test_v4l2_decoder_source_vicodec PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_v4l2_decoder_source_vicodec)

add_executable(test_page_flip unit/test_page_flip.cpp)
target_link_libraries(test_page_flip PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
gtest_discover_tests(test_page_flip)

# Connector-rank helper test. The header lives under examples/common,
# so the test target adds examples/ to its include path to resolve
# "common/select_connector.hpp".
add_executable(test_select_connector unit/test_select_connector.cpp)
target_link_libraries(test_select_connector PRIVATE
  drm-cxx::drm-cxx
  GTest::gtest
  GTest::gtest_main)
target_include_directories(test_select_connector PRIVATE
  ${PROJECT_SOURCE_DIR}/examples)
gtest_discover_tests(test_select_connector)

# signage_player playlist parser. signage_playlist still lives under
# examples/, so the playlist test only builds when examples do.
if(DRM_CXX_BUILD_EXAMPLES)
  add_executable(test_playlist unit/test_playlist.cpp)
  target_link_libraries(test_playlist PRIVATE
    signage_playlist
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(test_playlist)

  # test_patterns painters — same arrangement as the playlist test above.
  add_executable(test_test_patterns unit/test_test_patterns.cpp)
  target_link_libraries(test_test_patterns PRIVATE
    test_patterns_painters
    GTest::gtest
    GTest::gtest_main)
  gtest_discover_tests(test_test_patterns)
endif()
