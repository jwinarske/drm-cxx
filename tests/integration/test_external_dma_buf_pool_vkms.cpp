// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Behavioral coverage for ExternalDmaBufPool: lazy first-sight import keyed by
// buffer identity, acquire / idle-hold / advance, and per-key release. Allocates
// dumb buffers on a dumb-capable modeset card, exports each as a PRIME fd, and
// submits them by key. Self-skips when no such card is present (so it stays
// green on machines without one); it drives no KMS commit, so it needs neither a
// connected output nor DRM master beyond dumb-buffer + AddFB2 access.

#include "core/device.hpp"

#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/scene/external_dma_buf_pool.hpp>

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
#include <vector>

namespace {

constexpr std::uint32_t k_w = 320;
constexpr std::uint32_t k_h = 240;
constexpr std::size_t k_bufs = 2;

// A modeset card plus k_bufs dumb buffers, each exported as a PRIME fd that
// imports cleanly as a KMS framebuffer. nullopt if no such card is present.
struct Probe {
  drm::Device dev;
  std::vector<drm::dumb::Buffer> buffers;
  std::vector<int> dmabuf_fds;  // owned; closed in the destructor
  std::uint32_t stride{0};

  ~Probe() {
    for (int const fd : dmabuf_fds) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }
  Probe(drm::Device d, std::uint32_t s) : dev(std::move(d)), stride(s) {}
  Probe(const Probe&) = delete;
  Probe& operator=(const Probe&) = delete;
  Probe(Probe&&) = default;
  Probe& operator=(Probe&&) = delete;
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
    cfg.add_fb = false;  // the pool makes the FB from the imported handle
    auto first = drm::dumb::Buffer::create(*dev, cfg);
    if (!first) {
      continue;
    }
    int first_fd = -1;
    if (drmPrimeHandleToFD(dev->fd(), first->handle(), O_CLOEXEC | O_RDWR, &first_fd) != 0 ||
        first_fd < 0) {
      continue;
    }
    const std::uint32_t stride = first->stride();
    // Confirm the PRIME fd imports as a KMS FB via a one-shot pool before
    // committing to this card.
    auto probe_pool = drm::scene::ExternalDmaBufPool::create(*dev, k_w, k_h, DRM_FORMAT_XRGB8888,
                                                             DRM_FORMAT_MOD_LINEAR);
    if (!probe_pool) {
      ::close(first_fd);
      continue;
    }
    const std::array<drm::scene::ExternalPlaneInfo, 1> pp{
        drm::scene::ExternalPlaneInfo{first_fd, 0, stride}};
    probe_pool.value()->submit(
        0x1, drm::span<const drm::scene::ExternalPlaneInfo>(pp.data(), pp.size()));
    if (probe_pool.value()->cached_count() != 1) {
      ::close(first_fd);
      continue;
    }

    Probe p(std::move(*dev), stride);
    p.buffers.push_back(std::move(*first));
    p.dmabuf_fds.push_back(first_fd);
    bool ok = true;
    for (std::size_t s = 1; s < k_bufs; ++s) {
      auto buf = drm::dumb::Buffer::create(p.dev, cfg);
      if (!buf) {
        ok = false;
        break;
      }
      int fd = -1;
      if (drmPrimeHandleToFD(p.dev.fd(), buf->handle(), O_CLOEXEC | O_RDWR, &fd) != 0 || fd < 0) {
        ok = false;
        break;
      }
      p.buffers.push_back(std::move(*buf));
      p.dmabuf_fds.push_back(fd);
    }
    if (!ok) {
      continue;
    }
    return p;
  }
  return std::nullopt;
}

drm::span<const drm::scene::ExternalPlaneInfo> one(
    std::array<drm::scene::ExternalPlaneInfo, 1>& store, int fd, std::uint32_t stride) {
  store[0] = drm::scene::ExternalPlaneInfo{fd, 0, stride};
  return {store.data(), store.size()};
}

}  // namespace

TEST(ExternalDmaBufPoolVkms, LazyImportCachesByKey) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }
  auto pool = drm::scene::ExternalDmaBufPool::create(probe->dev, k_w, k_h, DRM_FORMAT_XRGB8888,
                                                     DRM_FORMAT_MOD_LINEAR);
  ASSERT_TRUE(pool.has_value()) << pool.error().message();

  constexpr std::uintptr_t k_key_a = 0xA;
  constexpr std::uintptr_t k_key_b = 0xB;
  std::array<drm::scene::ExternalPlaneInfo, 1> pa{};
  std::array<drm::scene::ExternalPlaneInfo, 1> pb{};

  (*pool)->submit(k_key_a, one(pa, probe->dmabuf_fds[0], probe->stride));
  EXPECT_EQ((*pool)->cached_count(), 1U);
  // Re-submitting the same key reuses the cached import — no second FB.
  (*pool)->submit(k_key_a, one(pa, probe->dmabuf_fds[0], probe->stride));
  EXPECT_EQ((*pool)->cached_count(), 1U);
  // A new key imports on first sight.
  (*pool)->submit(k_key_b, one(pb, probe->dmabuf_fds[1], probe->stride));
  EXPECT_EQ((*pool)->cached_count(), 2U);
}

TEST(ExternalDmaBufPoolVkms, AcquireAdvancesThenHoldsIdle) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }
  auto pool = drm::scene::ExternalDmaBufPool::create(probe->dev, k_w, k_h, DRM_FORMAT_XRGB8888,
                                                     DRM_FORMAT_MOD_LINEAR);
  ASSERT_TRUE(pool.has_value()) << pool.error().message();
  std::array<drm::scene::ExternalPlaneInfo, 1> pa{};
  std::array<drm::scene::ExternalPlaneInfo, 1> pb{};

  (*pool)->submit(0xA, one(pa, probe->dmabuf_fds[0], probe->stride));
  auto a1 = (*pool)->acquire();
  ASSERT_TRUE(a1.has_value()) << a1.error().message();
  EXPECT_NE(a1->fb_id, 0U);
  const std::uint32_t fb_a = a1->fb_id;

  // No fresh submit: acquire idle-holds the last good buffer (same FB), not EAGAIN.
  EXPECT_FALSE((*pool)->has_fresh_content());
  auto hold = (*pool)->acquire();
  ASSERT_TRUE(hold.has_value()) << hold.error().message();
  EXPECT_EQ(hold->fb_id, fb_a);

  // A fresh key advances to a different FB.
  (*pool)->submit(0xB, one(pb, probe->dmabuf_fds[1], probe->stride));
  EXPECT_TRUE((*pool)->has_fresh_content());
  auto a2 = (*pool)->acquire();
  ASSERT_TRUE(a2.has_value()) << a2.error().message();
  EXPECT_NE(a2->fb_id, 0U);
  EXPECT_NE(a2->fb_id, fb_a);
}

TEST(ExternalDmaBufPoolVkms, ReleaseFiresForDisplacedKey) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }
  std::vector<std::uintptr_t> released;
  drm::scene::ExternalDmaBufPool::Options opts;
  opts.on_release = [&](std::uintptr_t key, std::optional<drm::sync::SyncFence> /*f*/) {
    released.push_back(key);
  };
  auto pool = drm::scene::ExternalDmaBufPool::create(probe->dev, k_w, k_h, DRM_FORMAT_XRGB8888,
                                                     DRM_FORMAT_MOD_LINEAR, std::move(opts));
  ASSERT_TRUE(pool.has_value()) << pool.error().message();
  EXPECT_TRUE((*pool)->wants_release_fence());
  std::array<drm::scene::ExternalPlaneInfo, 1> pa{};
  std::array<drm::scene::ExternalPlaneInfo, 1> pb{};

  (*pool)->submit(0xA, one(pa, probe->dmabuf_fds[0], probe->stride));
  auto a1 = (*pool)->acquire();
  ASSERT_TRUE(a1.has_value());

  // Key B displaces A on the next acquire; A's acquisition token is now superseded.
  (*pool)->submit(0xB, one(pb, probe->dmabuf_fds[1], probe->stride));
  auto a2 = (*pool)->acquire();
  ASSERT_TRUE(a2.has_value());

  // Retiring A now fires its per-key release (it is off-screen).
  (*pool)->release_with_fence(std::move(*a1), std::nullopt);
  ASSERT_EQ(released.size(), 1U);
  EXPECT_EQ(released[0], 0xAU);

  // Retiring B must NOT fire: it is still the live/scanning frame.
  (*pool)->release_with_fence(std::move(*a2), std::nullopt);
  EXPECT_EQ(released.size(), 1U);
}
