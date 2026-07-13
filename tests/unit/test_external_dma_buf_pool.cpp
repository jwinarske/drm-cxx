// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit coverage for ExternalDmaBufPool::create() argument validation. Behavioral
// coverage (lazy import, acquire/idle-hold, release, export) needs a real card
// and lives in test_external_dma_buf_pool_vkms.cpp.

#include "core/device.hpp"

#include <drm-cxx/scene/external_dma_buf_pool.hpp>

#include <drm_fourcc.h>

#include <cstdint>
#include <gtest/gtest.h>
#include <system_error>

namespace {

constexpr std::uint32_t k_w = 320;
constexpr std::uint32_t k_h = 240;

TEST(SceneExternalDmaBufPool, RejectsZeroWidth) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::ExternalDmaBufPool::create(dev, /*width=*/0, k_h, DRM_FORMAT_ARGB8888,
                                                  DRM_FORMAT_MOD_LINEAR);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufPool, RejectsZeroHeight) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::ExternalDmaBufPool::create(dev, k_w, /*height=*/0, DRM_FORMAT_ARGB8888,
                                                  DRM_FORMAT_MOD_LINEAR);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufPool, RejectsZeroFourcc) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::ExternalDmaBufPool::create(dev, k_w, k_h, /*drm_fourcc=*/0,
                                                  DRM_FORMAT_MOD_LINEAR);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufPool, RejectsBadDeviceFd) {
  // Args valid, but the device carries a dead fd.
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::ExternalDmaBufPool::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888,
                                                  DRM_FORMAT_MOD_LINEAR);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::bad_file_descriptor));
}

}  // namespace
