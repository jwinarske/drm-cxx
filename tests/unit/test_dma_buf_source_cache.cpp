// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::DmaBufSourceCache covering the map logic visible
// without a live KMS device: empty-state queries and that a failed create()
// caches nothing. The cache-hit / staleness-recreate path (which needs a real
// imported DMA-BUF) lives in the vgem integration test.

#include "core/device.hpp"
#include "scene/dma_buf_source_cache.hpp"

#include <drm-cxx/scene/external_dma_buf_source.hpp>

#include <drm_fourcc.h>

#include <array>
#include <cstdint>
#include <gtest/gtest.h>

namespace {

constexpr std::uint32_t k_w = 320;
constexpr std::uint32_t k_h = 240;

drm::scene::ExternalPlaneInfo plane(int fd) {
  drm::scene::ExternalPlaneInfo p;
  p.fd = fd;
  p.offset = 0;
  p.pitch = k_w * 4;
  return p;
}

}  // namespace

TEST(DmaBufSourceCache, EmptyState) {
  drm::scene::DmaBufSourceCache cache;
  EXPECT_EQ(cache.size(), 0U);
  EXPECT_EQ(cache.find(0x1234U), nullptr);
  cache.evict(0x1234U);  // no-op on a missing key
  cache.clear();         // no-op when empty
  EXPECT_EQ(cache.size(), 0U);
}

TEST(DmaBufSourceCache, FailedCreateIsNotCached) {
  // An invalid device + a rejected argument make ExternalDmaBufSource::create
  // fail; the cache must propagate the error and store nothing.
  auto dev = drm::Device::from_fd(-1);
  drm::scene::DmaBufSourceCache cache;
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{plane(0)};
  auto r = cache.get_or_create(0xABCDU, dev, /*width=*/0, k_h, DRM_FORMAT_ARGB8888,
                               DRM_FORMAT_MOD_LINEAR, planes);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(cache.size(), 0U);
  EXPECT_EQ(cache.find(0xABCDU), nullptr);
}
