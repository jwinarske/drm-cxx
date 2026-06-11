// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration test for drm::scene::DmaBufSourceCache against a real imported
// DMA-BUF. Allocates a dumb buffer on a dumb-capable *modeset* card, exports it
// via PRIME, and drives the cache through it: cache-hit reuse, staleness
// recreation, and eviction. Needs a card where the full
// CREATE_DUMB → PRIME → drmModeAddFB2 round-trip works (vkms in CI; amdgpu /
// rockchip on hardware). Self-skips otherwise — vgem in particular is not
// DRIVER_MODESET, so AddFB2 can't run there.

#include "core/device.hpp"

#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/scene/dma_buf_source_cache.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include <array>
#include <cstdint>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::uint32_t k_w = 64;
constexpr std::uint32_t k_h = 64;

// A card + a dumb buffer whose PRIME fd imports cleanly as a KMS framebuffer
// (i.e. the cache's get_or_create succeeds). nullopt if no such card is present.
struct Probe {
  drm::Device dev;
  drm::dumb::Buffer buf;
  int dmabuf_fd;
};

std::optional<Probe> find_usable_card() {
  for (int i = 0; i < 8; ++i) {
    const std::string path = "/dev/dri/card" + std::to_string(i);
    if (::access(path.c_str(), R_OK | W_OK) != 0) {
      continue;
    }
    auto dev = drm::Device::open(path);
    if (!dev) {
      continue;
    }
    drm::dumb::Config cfg;
    cfg.width = k_w;
    cfg.height = k_h;
    cfg.drm_format = DRM_FORMAT_XRGB8888;
    cfg.bpp = 32;
    cfg.add_fb = false;  // we only need the BO + its PRIME fd; the cache makes the FB
    auto buf = drm::dumb::Buffer::create(*dev, cfg);
    if (!buf) {
      continue;
    }
    int fd = -1;
    if (drmPrimeHandleToFD(dev->fd(), buf->handle(), O_CLOEXEC | O_RDWR, &fd) != 0 || fd < 0) {
      continue;
    }
    // Confirm the whole round-trip works on this card before committing to it:
    // ExternalDmaBufSource::create runs the PRIME import + drmModeAddFB2.
    std::array<drm::scene::ExternalPlaneInfo, 1> planes{
        drm::scene::ExternalPlaneInfo{fd, 0, buf->stride()}};
    auto probe = drm::scene::ExternalDmaBufSource::create(*dev, k_w, k_h, DRM_FORMAT_XRGB8888,
                                                          DRM_FORMAT_MOD_LINEAR, planes);
    if (!probe) {
      ::close(fd);
      continue;
    }
    return Probe{std::move(*dev), std::move(*buf), fd};
  }
  return std::nullopt;
}

drm::scene::ExternalPlaneInfo plane_of(const drm::dumb::Buffer& buf, int fd) {
  return drm::scene::ExternalPlaneInfo{fd, 0, buf.stride()};
}

}  // namespace

TEST(DmaBufSourceCacheVkms, HitStalenessEvict) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }
  auto& dev = probe->dev;
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{plane_of(probe->buf, probe->dmabuf_fd)};
  constexpr std::uintptr_t k_key = 0xBEEF;

  drm::scene::DmaBufSourceCache cache;

  // First sight: creates + caches.
  auto a =
      cache.get_or_create(k_key, dev, k_w, k_h, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_TRUE(a.has_value()) << a.error().message();
  EXPECT_NE(*a, nullptr);
  EXPECT_EQ(cache.size(), 1U);
  EXPECT_EQ(cache.find(k_key), *a);

  // Cache hit: same key + same geometry returns the *same* source (fb_id reused).
  auto b =
      cache.get_or_create(k_key, dev, k_w, k_h, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_TRUE(b.has_value()) << b.error().message();
  EXPECT_EQ(*a, *b);
  EXPECT_EQ(cache.size(), 1U);

  // Staleness: same key, different geometry recreates (the key was reused for a
  // different buffer). Smaller dims so the existing dumb buffer still backs it.
  // Check the new geometry stuck rather than pointer identity — malloc may reuse
  // the just-freed source's address, so the pointer can legitimately match.
  auto c = cache.get_or_create(k_key, dev, k_w / 2, k_h / 2, DRM_FORMAT_XRGB8888,
                               DRM_FORMAT_MOD_LINEAR, planes);
  ASSERT_TRUE(c.has_value()) << c.error().message();
  EXPECT_EQ((*c)->format().width, k_w / 2);
  EXPECT_EQ((*c)->format().height, k_h / 2);
  EXPECT_EQ(cache.size(), 1U);

  // Evict + clear.
  cache.evict(k_key);
  EXPECT_EQ(cache.size(), 0U);
  EXPECT_EQ(cache.find(k_key), nullptr);
  cache.clear();
  EXPECT_EQ(cache.size(), 0U);

  ::close(probe->dmabuf_fd);
}
