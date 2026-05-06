// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::V4l2DecoderSource. This TU covers the
// argument-validation portion of the contract plus the open+QUERYCAP
// failure paths (path doesn't exist, fd isn't a V4L2 device).
//
// The full V4L2 round-trip (REQBUFS + MMAP + EXPBUF +
// drmPrimeFDToHandle + drmModeAddFB2 + acquire/release +
// submit_bitstream) is integration-test territory and lives in a
// follow-up against vicodec (the kernel's V4L2 virtual codec driver)
// once the source's buffer path lands.

#include "core/device.hpp"

#include <drm-cxx/scene/v4l2_decoder_source.hpp>

#include <cstdint>
#include <gtest/gtest.h>
#include <system_error>

namespace {

constexpr std::uint32_t k_codec_h264 = 0x34363248U;    // V4L2_PIX_FMT_H264 ('H','2','6','4')
constexpr std::uint32_t k_capture_nv12 = 0x3231564EU;  // V4L2_PIX_FMT_NV12 ('N','V','1','2')

drm::scene::V4l2DecoderConfig good_config() noexcept {
  drm::scene::V4l2DecoderConfig cfg;
  cfg.codec_fourcc = k_codec_h264;
  cfg.capture_fourcc = k_capture_nv12;
  cfg.coded_width = 1920;
  cfg.coded_height = 1080;
  cfg.output_buffer_count = 4;
  cfg.capture_buffer_count = 4;
  return cfg;
}

}  // namespace

TEST(SceneV4l2DecoderSource, RejectsNullDevicePath) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::V4l2DecoderSource::create(dev, nullptr, good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2DecoderSource, RejectsEmptyDevicePath) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::V4l2DecoderSource::create(dev, "", good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2DecoderSource, RejectsZeroCodecFourcc) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.codec_fourcc = 0;
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2DecoderSource, RejectsZeroCaptureFourcc) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.capture_fourcc = 0;
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2DecoderSource, RejectsZeroCodedWidth) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.coded_width = 0;
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2DecoderSource, RejectsZeroCodedHeight) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.coded_height = 0;
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2DecoderSource, RejectsTooFewOutputBuffers) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.output_buffer_count = 1;
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2DecoderSource, RejectsTooManyOutputBuffers) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.output_buffer_count = 33;
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2DecoderSource, RejectsTooFewCaptureBuffers) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.capture_buffer_count = 1;
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneV4l2DecoderSource, RejectsTooManyCaptureBuffers) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = good_config();
  cfg.capture_buffer_count = 33;
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/video10", cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

// Path that is guaranteed not to resolve (the parent directory does
// not exist on any sane system), so create() reaches ::open and gets
// ENOENT. Pins the "valid args, real failure surfaces verbatim"
// contract without depending on whether /dev/video* nodes happen to be
// present on the host running tests.
TEST(SceneV4l2DecoderSource, ValidConfigPropagatesOpenFailure) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/drm-cxx/no-such-v4l2-device-for-test",
                                                 good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::no_such_file_or_directory));
}

// /dev/null opens successfully but isn't a V4L2 device, so VIDIOC_QUERYCAP
// returns ENOTTY. The source translates that to errc::not_supported so
// callers can distinguish "wrong path" (ENOENT above) from "right path,
// wrong device".
TEST(SceneV4l2DecoderSource, NonV4l2DeviceSurfacesAsNotSupported) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/null", good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::not_supported));
}
