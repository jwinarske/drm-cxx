// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::dumb::Buffer. Covers the contract visible without
// a live KMS device: default-constructed state, move semantics, config
// validation, graceful failure on non-DRM fds. A round-trip allocation
// against vkms belongs in an integration test (see the test_capture_vkms
// pattern) and is not required to shake out this TU.

#include "core/device.hpp"
#include "dumb/buffer.hpp"

#include <drm_fourcc.h>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>
#include <utility>

namespace {

drm::dumb::Config valid_argb_config(std::uint32_t w = 64, std::uint32_t h = 64) {
  drm::dumb::Config cfg;
  cfg.width = w;
  cfg.height = h;
  cfg.drm_format = DRM_FORMAT_ARGB8888;
  cfg.bpp = 32;
  cfg.add_fb = true;
  return cfg;
}

}  // namespace

TEST(DumbBuffer, DefaultCtorIsEmpty) {
  drm::dumb::Buffer b;
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.width(), 0U);
  EXPECT_EQ(b.height(), 0U);
  EXPECT_EQ(b.stride(), 0U);
  EXPECT_EQ(b.handle(), 0U);
  EXPECT_EQ(b.fb_id(), 0U);
  EXPECT_EQ(b.size_bytes(), 0U);
  EXPECT_EQ(b.data(), nullptr);
}

TEST(DumbBuffer, ForgetOnEmptyIsSafe) {
  drm::dumb::Buffer b;
  b.forget();  // must not crash, must leave empty() true
  EXPECT_TRUE(b.empty());
}

TEST(DumbBuffer, MoveConstructTransfersOwnership) {
  drm::dumb::Buffer src;
  drm::dumb::Buffer dst = std::move(src);
  EXPECT_TRUE(dst.empty());
  EXPECT_TRUE(src.empty());  // NOLINT(bugprone-use-after-move) — checking the contract
}

TEST(DumbBuffer, MoveAssignReplacesTarget) {
  drm::dumb::Buffer a;
  drm::dumb::Buffer b;
  a = std::move(b);
  EXPECT_TRUE(a.empty());
  EXPECT_TRUE(b.empty());  // NOLINT(bugprone-use-after-move) — checking the contract
}

TEST(DumbBuffer, CreateRejectsZeroWidth) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = valid_argb_config();
  cfg.width = 0;
  auto r = drm::dumb::Buffer::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(DumbBuffer, CreateRejectsZeroHeight) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = valid_argb_config();
  cfg.height = 0;
  auto r = drm::dumb::Buffer::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(DumbBuffer, CreateRejectsZeroFormat) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = valid_argb_config();
  cfg.drm_format = 0;
  auto r = drm::dumb::Buffer::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(DumbBuffer, CreateRejectsZeroBpp) {
  auto dev = drm::Device::from_fd(-1);
  auto cfg = valid_argb_config();
  cfg.bpp = 0;
  auto r = drm::dumb::Buffer::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(DumbBuffer, CreateOnInvalidFdFailsGracefully) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::dumb::Buffer::create(dev, valid_argb_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::bad_file_descriptor));
}

TEST(DumbBuffer, CreateOnNonDrmFdFailsGracefully) {
  // /dev/null accepts fd operations but rejects DRM ioctls. The factory
  // must return an error rather than crashing or leaking.
  const int fd = ::open("/dev/null", O_RDWR);
  if (fd < 0) {
    GTEST_SKIP() << "Cannot open /dev/null";
  }
  auto dev = drm::Device::from_fd(fd);
  auto r = drm::dumb::Buffer::create(dev, valid_argb_config());
  EXPECT_FALSE(r.has_value());
  ::close(fd);
}
