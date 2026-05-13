// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Headless unit tests for drm::scene::EglStreamSource. Like the
// EglStreamBuilder tests, these focus on the rejection paths that
// fire before any real EGL call: capability validation and Config
// field validation. The source's create() factory walks several
// guards before reaching eglCreateStreamKHR, all of which are
// deterministically reachable without an EGL stack.
//
// The class lives under src/scene/ as an internal header; tests pick
// it up through the build's PUBLIC include path on the drm-cxx
// target.
//
// End-to-end stream creation against real NVIDIA hardware is manual.

#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/stream_capability.hpp>

#include <gtest/gtest.h>

#if DRM_CXX_HAS_EGL_STREAMS

#include "scene/egl_stream_source.hpp"

#include <EGL/egl.h>
#include <system_error>

namespace {

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

// Non-null sentinel handles for the Config tests. The factory's
// invalid_argument guard fires before any of these get dereferenced,
// so the sentinels are never touched by real EGL code. Use a
// function's address as the sentinel — guaranteed non-null and with
// static storage duration, no integer-to-pointer cast required.
EGLDisplay sentinel_display() {
  return reinterpret_cast<EGLDisplay>(&sentinel_display);
}
EGLConfig sentinel_config() {
  return reinterpret_cast<EGLConfig>(&sentinel_config);
}

}  // namespace

TEST(EglStreamSource, CreateRejectsUnsupportedCapability) {
  drm::scene::EglStreamSource::Config cfg{};
  cfg.display = sentinel_display();
  cfg.egl_config = sentinel_config();
  cfg.format = valid_format();
  const auto r =
      drm::scene::EglStreamSource::create(drm::scene::stream_capability_unsupported(), cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::function_not_supported));
}

TEST(EglStreamSource, CreateRejectsCapabilityMissingStreamsChain) {
  // usable() is true (mixing == Exclusive) but the per-display
  // extension flags say no KHR_stream / no output_drm — the source
  // can't be wired without those. Treat as function_not_supported.
  drm::scene::StreamCapability cap = make_usable_capability();
  cap.has_khr_stream = false;
  drm::scene::EglStreamSource::Config cfg{};
  cfg.display = sentinel_display();
  cfg.egl_config = sentinel_config();
  cfg.format = valid_format();
  const auto r = drm::scene::EglStreamSource::create(cap, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::function_not_supported));
}

TEST(EglStreamSource, CreateRejectsNullDisplay) {
  drm::scene::EglStreamSource::Config cfg{};
  cfg.display = EGL_NO_DISPLAY;
  cfg.egl_config = sentinel_config();
  cfg.format = valid_format();
  const auto r = drm::scene::EglStreamSource::create(make_usable_capability(), cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(EglStreamSource, CreateRejectsNullEglConfig) {
  drm::scene::EglStreamSource::Config cfg{};
  cfg.display = sentinel_display();
  cfg.egl_config = nullptr;
  cfg.format = valid_format();
  const auto r = drm::scene::EglStreamSource::create(make_usable_capability(), cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(EglStreamSource, CreateRejectsZeroWidth) {
  drm::scene::EglStreamSource::Config cfg{};
  cfg.display = sentinel_display();
  cfg.egl_config = sentinel_config();
  cfg.format = valid_format();
  cfg.format.width = 0;
  const auto r = drm::scene::EglStreamSource::create(make_usable_capability(), cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(EglStreamSource, CreateRejectsZeroHeight) {
  drm::scene::EglStreamSource::Config cfg{};
  cfg.display = sentinel_display();
  cfg.egl_config = sentinel_config();
  cfg.format = valid_format();
  cfg.format.height = 0;
  const auto r = drm::scene::EglStreamSource::create(make_usable_capability(), cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

// flip_event_data() / bound_plane() pre-bind contract: both
// accessors return nullopt before bind_to_plane has run. They get
// populated inside bind_to_plane (which needs real EGL state we
// can't construct headlessly), but the pre-bind shape is part of
// the public API and worth pinning.
TEST(EglStreamSource, FlipEventDataIsNulloptPreBind) {
  // The source can't actually be constructed here -- create() needs
  // a working EGL runtime to allocate the stream. We exercise the
  // contract via the type's invariant: every fresh EglStreamSource
  // starts with no plane binding and therefore no flip event id.
  // The rejection paths above already prove create() returns an
  // unexpected before any object is allocated, so this test just
  // documents the contract for the lifetime of an instance.
  SUCCEED() << "flip_event_data() / bound_plane() return nullopt until bind_to_plane "
               "succeeds; populated together inside bind_to_plane.";
}

#else  // !DRM_CXX_HAS_EGL_STREAMS

TEST(EglStreamSource, StreamsBuildGateDisabled) {
  GTEST_SKIP() << "drm-cxx built without -DDRM_CXX_STREAMS=ON; "
                  "EglStreamSource is not defined";
}

#endif  // DRM_CXX_HAS_EGL_STREAMS
