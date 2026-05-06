// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::GstAppsinkSource. This TU covers the
// argument-validation portion of the contract — every test runs
// against `Device::from_fd(-1)` so nothing actually opens a GStreamer
// pipeline.
//
// The full appsink round-trip (sample pull, DMABUF zero-copy import,
// system-memory memcpy fallback, latest-frame-wins drop semantics, bus
// pumping) is integration-test territory and lives in a follow-up
// against `videotestsrc ! appsink` once the source's runtime path
// lands.

#include "core/device.hpp"

#include <drm-cxx/scene/gst_appsink_source.hpp>

#include <gtest/gtest.h>
#include <system_error>

#if DRM_CXX_HAS_GSTREAMER
#include <gst/gst.h>                // gst_init
#include <gst/gstelementfactory.h>  // gst_element_factory_make
#include <gst/gstobject.h>          // gst_object_unref

namespace {

// Create a real GstAppSink element so the type pointer is meaningful;
// the source still rejects construction at the runtime stub. Pipeline
// is never started — `gst_element_factory_make` is enough to produce
// a valid `GstElement*`.
struct GstFixture : ::testing::Test {
  static void SetUpTestSuite() { gst_init(nullptr, nullptr); }

  void SetUp() override {
    appsink = gst_element_factory_make("appsink", "test_sink");
    ASSERT_NE(appsink, nullptr) << "appsink element factory missing — "
                                   "install gst-plugins-base";
  }
  void TearDown() override {
    if (appsink != nullptr) {
      gst_object_unref(appsink);
      appsink = nullptr;
    }
  }

  GstElement* appsink{nullptr};
};

}  // namespace

TEST_F(GstFixture, RejectsNullAppsink) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::GstAppsinkSource::create(dev, nullptr, {});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST_F(GstFixture, RejectsZeroFbCacheSize) {
  auto dev = drm::Device::from_fd(-1);
  drm::scene::GstAppsinkConfig cfg;
  cfg.fb_cache_size = 0;
  auto r = drm::scene::GstAppsinkSource::create(dev, appsink, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

// With appsink + cfg valid but the Device wrapping a -1 fd, create()
// runs past argument validation, gst_init, and the appsink type check,
// then trips on `dev.fd() < 0` and returns bad_file_descriptor.
// Catches API regressions in the validation order without needing a
// real DRM fd.
TEST_F(GstFixture, RejectsInvalidDeviceFd) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::GstAppsinkSource::create(dev, appsink, {});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::bad_file_descriptor));
}

// Non-appsink GstElement is rejected before the fd check. Uses a
// `fakesink` element factory which is always present in
// gst-plugins-base; the type-mismatch path returns invalid_argument.
TEST_F(GstFixture, RejectsNonAppsinkElement) {
  GstElement* fakesink = gst_element_factory_make("fakesink", "test_fakesink");
  ASSERT_NE(fakesink, nullptr);
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::GstAppsinkSource::create(dev, fakesink, {});
  EXPECT_FALSE(r.has_value());
  if (!r.has_value()) {
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
  }
  gst_object_unref(fakesink);
}

#else  // DRM_CXX_HAS_GSTREAMER

// In a stub build the create() returns function_not_supported with no
// arguments touched. Pin that so the build-without-gstreamer path is
// CI-tested rather than just visually inspected.
TEST(SceneGstAppsinkSourceStub, CreateReturnsFunctionNotSupported) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::GstAppsinkSource::create(dev, nullptr, {});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::function_not_supported));
}

#endif  // DRM_CXX_HAS_GSTREAMER
