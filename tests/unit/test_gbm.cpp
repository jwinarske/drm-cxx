// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::gbm::GbmDevice and drm::gbm::Buffer. Covers the
// contract visible without a live KMS device: default-constructed
// state, move semantics, config validation, graceful failure on
// non-DRM fds. A round-trip allocation against a real GPU belongs in
// an integration test.

#include "gbm/buffer.hpp"
#include "gbm/device.hpp"
#include "gbm/surface.hpp"

#include <drm_fourcc.h>
#include <gbm.h>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>
#include <utility>

namespace {

drm::gbm::Config valid_argb_config(std::uint32_t w = 64, std::uint32_t h = 64) {
  drm::gbm::Config cfg;
  cfg.width = w;
  cfg.height = h;
  cfg.drm_format = DRM_FORMAT_ARGB8888;
  cfg.usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE;
  cfg.add_fb = true;
  return cfg;
}

}  // namespace

TEST(GbmDeviceTest, CreateWithInvalidFdFails) {
  auto result = drm::gbm::GbmDevice::create(-1);
  EXPECT_FALSE(result.has_value());
}

TEST(GbmDeviceTest, CreateWithNonDrmFdGraceful) {
  // /dev/null is not a DRM device — gbm_create_device may or may not
  // succeed depending on the mesa implementation. Either way, no crash.
  int const fd = ::open("/dev/null", O_RDWR);
  if (fd < 0) {
    GTEST_SKIP() << "Cannot open /dev/null";
  }

  auto result = drm::gbm::GbmDevice::create(fd);
  // Just verify no crash — result may be valid or error
  (void)result;
  ::close(fd);
}

TEST(GbmDeviceTest, CreateWithRealDrmDevice) {
  auto dev_result = drm::gbm::GbmDevice::create(::open("/dev/dri/card0", O_RDWR));
  if (!dev_result.has_value()) {
    GTEST_SKIP() << "No DRM device available for GBM";
  }

  EXPECT_NE(dev_result->raw(), nullptr);
}

TEST(GbmDeviceTest, MoveConstruct) {
  auto dev_result = drm::gbm::GbmDevice::create(::open("/dev/dri/card0", O_RDWR));
  if (!dev_result.has_value()) {
    GTEST_SKIP() << "No DRM device available";
  }

  auto dev2 = std::move(*dev_result);
  EXPECT_NE(dev2.raw(), nullptr);
}

TEST(GbmBuffer, DefaultCtorIsEmpty) {
  drm::gbm::Buffer b;
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.raw(), nullptr);
  EXPECT_EQ(b.handle(), 0U);
  EXPECT_EQ(b.width(), 0U);
  EXPECT_EQ(b.height(), 0U);
  EXPECT_EQ(b.stride(), 0U);
  EXPECT_EQ(b.format(), 0U);
  EXPECT_EQ(b.fb_id(), 0U);
  EXPECT_EQ(b.size_bytes(), 0U);
}

TEST(GbmBuffer, ForgetOnEmptyIsSafe) {
  drm::gbm::Buffer b;
  b.forget();
  EXPECT_TRUE(b.empty());
}

TEST(GbmBuffer, MoveConstructTransfersOwnership) {
  drm::gbm::Buffer src;
  drm::gbm::Buffer dst = std::move(src);
  EXPECT_TRUE(dst.empty());
  EXPECT_TRUE(src.empty());  // NOLINT(bugprone-use-after-move) — checking the contract
}

TEST(GbmBuffer, MoveAssignReplacesTarget) {
  drm::gbm::Buffer a;
  drm::gbm::Buffer b;
  a = std::move(b);
  EXPECT_TRUE(a.empty());
  EXPECT_TRUE(b.empty());  // NOLINT(bugprone-use-after-move) — checking the contract
}

TEST(GbmBuffer, CreateRejectsZeroWidth) {
  auto dev = drm::gbm::GbmDevice::create(::open("/dev/dri/card0", O_RDWR));
  if (!dev.has_value()) {
    GTEST_SKIP() << "No DRM device available for GBM";
  }
  auto cfg = valid_argb_config();
  cfg.width = 0;
  auto r = drm::gbm::Buffer::create(*dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(GbmBuffer, CreateRejectsZeroHeight) {
  auto dev = drm::gbm::GbmDevice::create(::open("/dev/dri/card0", O_RDWR));
  if (!dev.has_value()) {
    GTEST_SKIP() << "No DRM device available for GBM";
  }
  auto cfg = valid_argb_config();
  cfg.height = 0;
  auto r = drm::gbm::Buffer::create(*dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(GbmBuffer, CreateRejectsZeroFormat) {
  auto dev = drm::gbm::GbmDevice::create(::open("/dev/dri/card0", O_RDWR));
  if (!dev.has_value()) {
    GTEST_SKIP() << "No DRM device available for GBM";
  }
  auto cfg = valid_argb_config();
  cfg.drm_format = 0;
  auto r = drm::gbm::Buffer::create(*dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(GbmBuffer, CreateRejectsZeroUsage) {
  auto dev = drm::gbm::GbmDevice::create(::open("/dev/dri/card0", O_RDWR));
  if (!dev.has_value()) {
    GTEST_SKIP() << "No DRM device available for GBM";
  }
  auto cfg = valid_argb_config();
  cfg.usage = 0;
  auto r = drm::gbm::Buffer::create(*dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}
