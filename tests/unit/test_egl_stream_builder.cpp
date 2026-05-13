// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Headless unit tests for drm::scene::EglStreamBuilder. The build()
// call mostly drives real EGL — creating displays, choosing configs,
// creating contexts — which only succeeds against a host with
// libEGL.so.1 and a matching EGLDeviceEXT for the caller's
// drm::Device. CI doesn't carry either, so the tests here focus on
// the rejection paths that fire before any EGL call:
//
//   * Unsupported StreamCapability.
//   * Zero format width / height.
//
// The "null device without existing display" path is only
// deterministic when libEGL is present (otherwise the runtime check
// short-circuits with function_not_supported before the device check
// fires); that test asserts the call returns one of the two
// acceptable error codes.
//
// End-to-end builder validation against real NVIDIA hardware is
// manual; CI can't host the proprietary EGL stack.

#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/stream_capability.hpp>

#include <gtest/gtest.h>

#if DRM_CXX_HAS_EGL_STREAMS

#include <drm-cxx/scene/egl_stream_builder.hpp>

#include <EGL/egl.h>
#include <system_error>

namespace {

// Hand-built StreamCapability that satisfies `usable()` and reports
// every per-display extension the builder + source check. Lets the
// rejection-path tests reach the format / device / runtime guards
// without smuggling real EGL state in.
drm::scene::StreamCapability make_usable_capability() {
  drm::scene::StreamCapability cap;
  cap.mixing = drm::scene::StreamMixingMode::Exclusive;
  cap.has_egl_runtime = true;
  cap.has_platform_device = true;
  cap.has_device_drm = true;
  cap.has_output_drm = true;
  cap.has_khr_stream = true;
  cap.has_stream_consumer_egloutput = true;
  cap.has_stream_producer_eglsurface = true;
  return cap;
}

drm::scene::SourceFormat valid_format() {
  return drm::scene::SourceFormat{.drm_fourcc = 0x34325241U,  // ARGB8888
                                  .modifier = 0,
                                  .width = 16,
                                  .height = 16};
}

}  // namespace

TEST(EglStreamBuilder, RejectsUnsupportedCapability) {
  drm::scene::EglStreamBuilder::Request req;
  req.capability = drm::scene::stream_capability_unsupported();
  req.format = valid_format();
  const auto r = drm::scene::EglStreamBuilder::build(req);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::function_not_supported));
}

TEST(EglStreamBuilder, RejectsZeroWidth) {
  drm::scene::EglStreamBuilder::Request req;
  req.capability = make_usable_capability();
  req.format = valid_format();
  req.format.width = 0;
  const auto r = drm::scene::EglStreamBuilder::build(req);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(EglStreamBuilder, RejectsZeroHeight) {
  drm::scene::EglStreamBuilder::Request req;
  req.capability = make_usable_capability();
  req.format = valid_format();
  req.format.height = 0;
  const auto r = drm::scene::EglStreamBuilder::build(req);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(EglStreamBuilder, RejectsNullDeviceWithoutExistingDisplay) {
  drm::scene::EglStreamBuilder::Request req;
  req.capability = make_usable_capability();
  req.format = valid_format();
  req.device = nullptr;
  req.existing_display = EGL_NO_DISPLAY;
  const auto r = drm::scene::EglStreamBuilder::build(req);
  ASSERT_FALSE(r.has_value());
  // Two acceptable verdicts depending on host EGL state:
  //   * libEGL absent: rt.loaded is false, runtime check returns
  //     function_not_supported BEFORE the null-device check fires.
  //   * libEGL present: rt.loaded is true, runtime check passes, the
  //     null-device check fires and returns invalid_argument.
  // Both shapes are correct from the builder's contract; the test
  // proves we don't crash and we don't accidentally accept the
  // request.
  EXPECT_TRUE(r.error() == std::make_error_code(std::errc::invalid_argument) ||
              r.error() == std::make_error_code(std::errc::function_not_supported));
}

#else  // !DRM_CXX_HAS_EGL_STREAMS

TEST(EglStreamBuilder, StreamsBuildGateDisabled) {
  GTEST_SKIP() << "drm-cxx built without -DDRM_CXX_STREAMS=ON; "
                  "EglStreamBuilder is not defined";
}

#endif  // DRM_CXX_HAS_EGL_STREAMS
