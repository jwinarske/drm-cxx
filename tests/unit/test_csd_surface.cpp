// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::csd::Surface. Covers the contract visible
// without a live KMS device: default-constructed state, move
// semantics, config validation, graceful failure on non-DRM fds.
// A round-trip allocation + paint pass against vkms belongs in an
// integration test (mirrors the test_dumb_buffer / test_capture_vkms
// split) and is not required to shake out this TU.

#include "core/device.hpp"
#include "csd/surface.hpp"

#include <drm_fourcc.h>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>
#include <utility>

namespace {

drm::csd::SurfaceConfig valid_config(std::uint32_t w = 64, std::uint32_t h = 64) {
  drm::csd::SurfaceConfig cfg;
  cfg.width = w;
  cfg.height = h;
  return cfg;
}

}  // namespace

TEST(CsdSurface, DefaultCtorIsEmpty) {
  drm::csd::Surface s;
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.fb_id(), 0U);
  EXPECT_EQ(s.width(), 0U);
  EXPECT_EQ(s.height(), 0U);
  EXPECT_EQ(s.stride(), 0U);
}

TEST(CsdSurface, FormatIsAlwaysArgb8888) {
  drm::csd::Surface s;
  EXPECT_EQ(s.format(), static_cast<std::uint32_t>(DRM_FORMAT_ARGB8888));
}

TEST(CsdSurface, ForgetOnEmptyIsSafe) {
  drm::csd::Surface s;
  s.forget();  // must not crash; must leave empty() true
  EXPECT_TRUE(s.empty());
}

TEST(CsdSurface, PaintOnEmptyReturnsError) {
  drm::csd::Surface s;
  auto m = s.paint();
  EXPECT_FALSE(m.has_value());
}

TEST(CsdSurface, DmaBufFdOnEmptyReturnsError) {
  drm::csd::Surface s;
  auto fd = s.dma_buf_fd();
  EXPECT_FALSE(fd.has_value());
}

TEST(CsdSurface, MoveCtorPreservesEmpty) {
  drm::csd::Surface a;
  drm::csd::Surface b{std::move(a)};
  EXPECT_TRUE(b.empty());
}

TEST(CsdSurface, MoveAssignPreservesEmpty) {
  drm::csd::Surface a;
  drm::csd::Surface b;
  b = std::move(a);
  EXPECT_TRUE(b.empty());
}

TEST(CsdSurface, CreateRejectsZeroWidth) {
  int devnull = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(devnull, 0);
  auto dev = drm::Device::from_fd(devnull);

  auto cfg = valid_config(0, 64);
  auto result = drm::csd::Surface::create(dev, cfg);
  EXPECT_FALSE(result.has_value());

  ::close(devnull);
}

TEST(CsdSurface, CreateRejectsZeroHeight) {
  int devnull = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(devnull, 0);
  auto dev = drm::Device::from_fd(devnull);

  auto cfg = valid_config(64, 0);
  auto result = drm::csd::Surface::create(dev, cfg);
  EXPECT_FALSE(result.has_value());

  ::close(devnull);
}

TEST(CsdSurface, CreateAgainstNonDrmFdFails) {
  // /dev/null isn't a DRM device, so the dumb path's CREATE_DUMB ioctl
  // returns ENOTTY. Surface should propagate that as an error rather
  // than crashing or returning a half-built buffer.
  int devnull = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(devnull, 0);
  auto dev = drm::Device::from_fd(devnull);

  auto result = drm::csd::Surface::create(dev, valid_config());
  EXPECT_FALSE(result.has_value());

  ::close(devnull);
}