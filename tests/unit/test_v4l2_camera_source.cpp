// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::V4l2CameraSource. This TU covers the
// argument-validation portion of the contract plus the open + QUERYCAP
// failure paths. The full V4L2 round-trip (REQBUFS + MMAP + EXPBUF +
// drmPrimeFDToHandle + drmModeAddFB2 + acquire/release) is integration-
// test territory and lives in a follow-up against vivid / a real UVC
// device.
//
// Note: V4L2_PIX_FMT_* are not constexpr in linux/videodev2.h (they're
// macros calling v4l2_fourcc()), so we restate the literal FourCC
// values here to keep this TU header-clean.

#include "core/device.hpp"

#include <drm-cxx/scene/v4l2_camera_source.hpp>

#include <cstdint>
#include <gtest/gtest.h>
#include <system_error>

namespace {

constexpr std::uint32_t k_pix_fmt_nv12 = 0x3231564EU;   // V4L2_PIX_FMT_NV12 ('N','V','1','2')
constexpr std::uint32_t k_pix_fmt_yuyv = 0x56595559U;   // V4L2_PIX_FMT_YUYV ('Y','U','Y','V')
constexpr std::uint32_t k_pix_fmt_mjpeg = 0x47504A4DU;  // V4L2_PIX_FMT_MJPEG ('M','J','P','G')

drm::scene::V4l2CameraConfig good_config() noexcept {
  drm::scene::V4l2CameraConfig cfg;
  cfg.pixel_fourcc = k_pix_fmt_nv12;
  cfg.width = 1280;
  cfg.height = 720;
  cfg.buffer_count = 4;
  cfg.mode = drm::scene::V4l2CameraBufferMode::Auto;
  cfg.modifier = 0;
  return cfg;
}

}  // namespace

TEST(SceneV4l2CameraSource, RejectsNullDevicePath) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::V4l2CameraSource::create(dev, nullptr, good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2CameraSource, RejectsEmptyDevicePath) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::V4l2CameraSource::create(dev, "", good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2CameraSource, RejectsZeroPixelFourcc) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.pixel_fourcc = 0;
  auto r = drm::scene::V4l2CameraSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2CameraSource, RejectsZeroWidth) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.width = 0;
  auto r = drm::scene::V4l2CameraSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2CameraSource, RejectsZeroHeight) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.height = 0;
  auto r = drm::scene::V4l2CameraSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2CameraSource, RejectsTooFewBuffers) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.buffer_count = 1;
  auto r = drm::scene::V4l2CameraSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2CameraSource, RejectsTooManyBuffers) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.buffer_count = 33;
  auto r = drm::scene::V4l2CameraSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

// MJPEG is explicitly out of v1 scope — it would require a decoder. The
// source surfaces this as not_supported up front rather than failing
// later with a confusing V4L2 error.
TEST(SceneV4l2CameraSource, RejectsUnsupportedFourcc) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.pixel_fourcc = k_pix_fmt_mjpeg;
  auto r = drm::scene::V4l2CameraSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::not_supported));
}

// YUYV is in scope; reaching the device-open step pins that the
// validator accepts it.
TEST(SceneV4l2CameraSource, AcceptsYUYVUntilOpen) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.pixel_fourcc = k_pix_fmt_yuyv;
  auto r =
      drm::scene::V4l2CameraSource::create(dev, "/dev/drm-cxx/no-such-v4l2-device-for-test", cfg);
  ASSERT_FALSE(r.has_value());
  // Reaches ::open and fails with ENOENT — confirming validation passed.
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::no_such_file_or_directory));
}

// Path that is guaranteed not to resolve (the parent directory does
// not exist on any sane system) so create() reaches ::open and gets
// ENOENT. Pins the "valid args, real failure surfaces verbatim"
// contract without depending on whether /dev/video* nodes happen to be
// present on the host.
TEST(SceneV4l2CameraSource, ValidConfigPropagatesOpenFailure) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::V4l2CameraSource::create(dev, "/dev/drm-cxx/no-such-v4l2-device-for-test",
                                                good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::no_such_file_or_directory));
}

// /dev/null opens successfully but isn't a V4L2 device, so VIDIOC_QUERYCAP
// returns ENOTTY. The source translates that to errc::not_supported.
TEST(SceneV4l2CameraSource, NonV4l2DeviceSurfacesAsNotSupported) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::V4l2CameraSource::create(dev, "/dev/null", good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::not_supported));
}
