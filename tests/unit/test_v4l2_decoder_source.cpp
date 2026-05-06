// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::V4l2DecoderSource. This TU covers the
// argument-validation portion of the contract — every test runs
// against `Device::from_fd(-1)` so nothing actually opens a V4L2
// device or hits the kernel.
//
// The full V4L2 round-trip (open + format negotiation + REQBUFS +
// MMAP + EXPBUF + drmPrimeFDToHandle + drmModeAddFB2 + acquire/release
// + submit_bitstream) is integration-test territory and lives in a
// follow-up against vivid (V4L2 virtual decoder) once the source's
// runtime path lands.

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

// With every field valid, the runtime path is still stubbed — create()
// returns function_not_supported once it reaches the V4L2 open. This
// test pins that contract until the runtime path lands; flip to
// has_value() / device_path-not-found expectations on the follow-up.
TEST(SceneV4l2DecoderSource, ValidConfigReachesRuntimeStub) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::V4l2DecoderSource::create(dev, "/dev/video10", good_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::function_not_supported));
}
