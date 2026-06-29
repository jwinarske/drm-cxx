// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration test for drm::scene::ExternalDmaBufRing against real imported
// DMA-BUFs. Allocates N dumb buffers on a dumb-capable *modeset* card, exports
// each via PRIME, builds a ring over them, and drives the slot FSM: submit ->
// acquire rotation, the idle hold-last-frame path, and the
// release-on-genuine-replacement-only signal. Also exercises output discovery
// (display::ScanoutTarget::discover) as the harness output-selection API.
//
// Needs a card where CREATE_DUMB -> PRIME -> drmModeAddFB2 works (vkms in CI;
// amdgpu / rockchip on hardware). Self-skips otherwise — vgem in particular is
// not DRIVER_MODESET, so AddFB2 can't run there.

#include "core/device.hpp"

#include <drm-cxx/display/scanout_target.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>  // DamageRect
#include <drm-cxx/scene/external_dma_buf_ring.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t k_w = 64;
constexpr std::uint32_t k_h = 64;
constexpr std::size_t k_slots = 2;

// A modeset card plus k_slots dumb buffers, each exported as a PRIME fd that
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
    // Build the first dumb buffer and confirm its PRIME fd imports as a KMS FB
    // via a one-slot ring before committing to this card.
    drm::dumb::Config cfg;
    cfg.width = k_w;
    cfg.height = k_h;
    cfg.drm_format = DRM_FORMAT_XRGB8888;
    cfg.bpp = 32;
    cfg.add_fb = false;  // the ring makes the FB from the imported handle
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
    std::array<drm::scene::ExternalPlaneInfo, 1> probe_planes{
        drm::scene::ExternalPlaneInfo{first_fd, 0, stride}};
    std::array<drm::scene::ExternalSlotDesc, 1> probe_slots{drm::scene::ExternalSlotDesc{
        DRM_FORMAT_MOD_LINEAR,
        drm::span<const drm::scene::ExternalPlaneInfo>(probe_planes.data(), probe_planes.size())}};
    auto probe_ring = drm::scene::ExternalDmaBufRing::create(
        *dev, k_w, k_h, DRM_FORMAT_XRGB8888,
        drm::span<const drm::scene::ExternalSlotDesc>(probe_slots.data(), probe_slots.size()));
    if (!probe_ring) {
      ::close(first_fd);
      continue;
    }
    probe_ring.value().reset();  // release the FB before re-importing for real

    // This card works. Allocate the full set of slots.
    Probe p(std::move(*dev), stride);
    p.buffers.push_back(std::move(*first));
    p.dmabuf_fds.push_back(first_fd);
    bool ok = true;
    for (std::size_t s = 1; s < k_slots; ++s) {
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

std::vector<drm::scene::ExternalSlotDesc> slots_of(
    const Probe& probe, std::vector<std::array<drm::scene::ExternalPlaneInfo, 1>>& storage) {
  storage.clear();
  storage.reserve(probe.dmabuf_fds.size());
  std::vector<drm::scene::ExternalSlotDesc> slots;
  for (int const fd : probe.dmabuf_fds) {
    storage.push_back({drm::scene::ExternalPlaneInfo{fd, 0, probe.stride}});
  }
  slots.reserve(storage.size());
  for (auto& planes : storage) {
    slots.push_back(drm::scene::ExternalSlotDesc{
        DRM_FORMAT_MOD_LINEAR,
        drm::span<const drm::scene::ExternalPlaneInfo>(planes.data(), planes.size())});
  }
  return slots;
}

}  // namespace

TEST(ExternalDmaBufRingVkms, RotatesSlotsAndHoldsIdle) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }

  std::vector<std::size_t> released;
  drm::scene::ExternalDmaBufRing::Options opts;
  opts.on_release = [&released](std::size_t slot, std::optional<drm::sync::SyncFence> fence) {
    EXPECT_FALSE(fence.has_value());  // callback-edge form until the scene wires OUT_FENCE
    released.push_back(slot);
  };

  std::vector<std::array<drm::scene::ExternalPlaneInfo, 1>> storage;
  auto slots = slots_of(*probe, storage);
  auto ring = drm::scene::ExternalDmaBufRing::create(
      probe->dev, k_w, k_h, DRM_FORMAT_XRGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()), std::move(opts));
  ASSERT_TRUE(ring.has_value()) << ring.error().message();
  auto& r = **ring;
  ASSERT_EQ(r.slot_count(), k_slots);

  // Nothing submitted yet: no frame to contribute.
  auto empty = r.acquire();
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error(), std::make_error_code(std::errc::resource_unavailable_try_again));

  // Submit slot 0, acquire it.
  r.submit(0);
  auto a0 = r.acquire();
  ASSERT_TRUE(a0.has_value()) << a0.error().message();
  EXPECT_NE(a0->fb_id, 0U);
  const std::uint32_t fb0 = a0->fb_id;

  // Submit slot 1, acquire it — a different FB.
  r.submit(1);
  auto a1 = r.acquire();
  ASSERT_TRUE(a1.has_value()) << a1.error().message();
  EXPECT_NE(a1->fb_id, 0U);
  EXPECT_NE(a1->fb_id, fb0);
  const std::uint32_t fb1 = a1->fb_id;

  // Retiring slot 0 now (it was replaced by slot 1) fires its release.
  r.release(std::move(*a0));
  ASSERT_EQ(released.size(), 1U);
  EXPECT_EQ(released[0], 0U);

  // Idle hold: no fresh submit -> acquire re-hands the scanning slot (1),
  // holding the last good frame instead of EAGAIN, and with no acquire fence.
  auto hold = r.acquire();
  ASSERT_TRUE(hold.has_value()) << hold.error().message();
  EXPECT_EQ(hold->fb_id, fb1);
  EXPECT_FALSE(hold->acquire_fence.has_value());

  // Releasing the held buffer must NOT signal free — slot 1 is still live.
  r.release(std::move(*hold));
  EXPECT_EQ(released.size(), 1U);

  // Advance to slot 0 again; retiring the still-outstanding slot-1 buffer now
  // fires its release (slot 1 was genuinely replaced).
  r.submit(0);
  auto a2 = r.acquire();
  ASSERT_TRUE(a2.has_value()) << a2.error().message();
  EXPECT_EQ(a2->fb_id, fb0);
  r.release(std::move(*a1));
  ASSERT_EQ(released.size(), 2U);
  EXPECT_EQ(released[1], 1U);

  r.release(std::move(*a2));
}

// submit(slot, fence, damage) round-trips the dirty-region list onto
// AcquiredBuffer::damage for the scene's FB_DAMAGE_CLIPS path, and honors the
// two contract rules: replace-not-union on re-submit, and over-cap -> whole-frame
// (empty). The idle hold-last-frame re-hand reports whole-frame (empty) too.
TEST(ExternalDmaBufRingVkms, CarriesDamageThroughAcquire) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }

  std::vector<std::array<drm::scene::ExternalPlaneInfo, 1>> storage;
  auto slots = slots_of(*probe, storage);
  auto ring = drm::scene::ExternalDmaBufRing::create(
      probe->dev, k_w, k_h, DRM_FORMAT_XRGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()));
  ASSERT_TRUE(ring.has_value()) << ring.error().message();
  auto& r = **ring;

  // (a) submit with damage -> acquire emits exactly those rects.
  const std::array<drm::scene::DamageRect, 2> rects{drm::scene::DamageRect{1, 2, 3, 4},
                                                    drm::scene::DamageRect{10, 20, 30, 40}};
  r.submit(0, std::nullopt, drm::span<const drm::scene::DamageRect>(rects.data(), rects.size()));
  auto a0 = r.acquire();
  ASSERT_TRUE(a0.has_value()) << a0.error().message();
  ASSERT_EQ(a0->damage.size(), 2U);
  EXPECT_EQ(a0->damage[0].x, 1);
  EXPECT_EQ(a0->damage[0].y, 2);
  EXPECT_EQ(a0->damage[0].w, 3U);
  EXPECT_EQ(a0->damage[0].h, 4U);
  EXPECT_EQ(a0->damage[1].x, 10);
  EXPECT_EQ(a0->damage[1].h, 40U);

  // (b) idle hold (no fresh submit) -> whole-frame (empty), not the last list.
  auto hold = r.acquire();
  ASSERT_TRUE(hold.has_value()) << hold.error().message();
  EXPECT_TRUE(hold->damage.empty());
  r.release(std::move(*hold));

  // (c) re-submit replaces, not unions: a single new rect wins outright.
  const std::array<drm::scene::DamageRect, 1> one{drm::scene::DamageRect{5, 6, 7, 8}};
  r.submit(1, std::nullopt, drm::span<const drm::scene::DamageRect>(one.data(), one.size()));
  auto a1 = r.acquire();
  ASSERT_TRUE(a1.has_value()) << a1.error().message();
  ASSERT_EQ(a1->damage.size(), 1U);
  EXPECT_EQ(a1->damage[0].x, 5);
  EXPECT_EQ(a1->damage[0].w, 7U);
  r.release(std::move(*a0));
  r.release(std::move(*a1));

  // (d) submit with no damage -> whole-frame (empty).
  r.submit(0);
  auto a2 = r.acquire();
  ASSERT_TRUE(a2.has_value()) << a2.error().message();
  EXPECT_TRUE(a2->damage.empty());

  // (e) over-cap (>16 rects) -> degrade to whole-frame (empty), never truncate.
  std::array<drm::scene::DamageRect, 17> too_many{};
  std::int32_t x = 0;
  for (auto& d : too_many) {
    d = drm::scene::DamageRect{x++, 0, 1, 1};
  }
  r.submit(1, std::nullopt,
           drm::span<const drm::scene::DamageRect>(too_many.data(), too_many.size()));
  auto a3 = r.acquire();
  ASSERT_TRUE(a3.has_value()) << a3.error().message();
  EXPECT_TRUE(a3->damage.empty());
  r.release(std::move(*a2));
  r.release(std::move(*a3));
}

// Fault isolation: a deadline-configured ring still advances normally when a submit
// carries no fence (CEF-style CPU-ready buffers) — the pre-wait is skipped, no
// IN_FENCE_FD is wired, and the slot rotates. The full timeout -> hold-last-frame
// -> auto-recover path needs a controllable producer fence (sw_sync / vgem) and
// is validated where one is available (CI / hardware), not on this host.
TEST(ExternalDmaBufRingVkms, DeadlineRingAdvancesWithoutFence) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }
  drm::scene::ExternalDmaBufRing::Options opts;
  opts.fence_deadline = std::chrono::milliseconds(5);

  std::vector<std::array<drm::scene::ExternalPlaneInfo, 1>> storage;
  auto slots = slots_of(*probe, storage);
  auto ring = drm::scene::ExternalDmaBufRing::create(
      probe->dev, k_w, k_h, DRM_FORMAT_XRGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()), std::move(opts));
  ASSERT_TRUE(ring.has_value()) << ring.error().message();
  auto& r = **ring;

  r.submit(0);  // no fence
  auto a0 = r.acquire();
  ASSERT_TRUE(a0.has_value()) << a0.error().message();
  EXPECT_NE(a0->fb_id, 0U);
  EXPECT_FALSE(a0->acquire_fence.has_value());  // never wire IN_FENCE_FD under a deadline
  const std::uint32_t fb0 = a0->fb_id;

  r.submit(1);
  auto a1 = r.acquire();
  ASSERT_TRUE(a1.has_value()) << a1.error().message();
  EXPECT_NE(a1->fb_id, fb0);  // advanced
  EXPECT_FALSE(a1->acquire_fence.has_value());

  r.release(std::move(*a0));
  r.release(std::move(*a1));
}

// Fault isolation timeout -> hold -> recover, end to end. A pipe stands in for the
// producer's render-done sync_file: SyncFence::wait() is a poll(POLLIN), so an
// empty pipe reads as "unsignaled" (the deadline wait times out) and a byte
// written to it as "signaled". This drives the exact fence->wait(deadline)
// branch the ring takes — no sw_sync/vgem (absent on stock kernels) needed; the
// FI path only wait()s, never merge()s, so a pipe is sound. A hung producer
// (fence never signals within the deadline) must HOLD the last good slot, not
// blank or stall the CRTC; once a fence signals again the next submit advances.
TEST(ExternalDmaBufRingVkms, FenceDeadlineMissHoldsThenRecovers) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }
  drm::scene::ExternalDmaBufRing::Options opts;
  opts.fence_deadline = std::chrono::milliseconds(30);

  std::vector<std::array<drm::scene::ExternalPlaneInfo, 1>> storage;
  auto slots = slots_of(*probe, storage);
  auto ring = drm::scene::ExternalDmaBufRing::create(
      probe->dev, k_w, k_h, DRM_FORMAT_XRGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()), std::move(opts));
  ASSERT_TRUE(ring.has_value()) << ring.error().message();
  auto& r = **ring;

  // Establish a live slot 0 (no fence: ready immediately).
  r.submit(0);
  auto a0 = r.acquire();
  ASSERT_TRUE(a0.has_value()) << a0.error().message();
  const std::uint32_t fb0 = a0->fb_id;
  ASSERT_NE(fb0, 0U);

  // Hung producer: submit slot 1 with a fence that never signals (empty pipe).
  std::array<int, 2> pipefd{-1, -1};
  ASSERT_EQ(::pipe(pipefd.data()), 0);
  auto unsignaled = drm::sync::SyncFence::import_fd(pipefd[0]);
  ASSERT_TRUE(unsignaled.has_value());
  r.submit(1, std::move(*unsignaled));

  auto held = r.acquire();  // CPU-waits 30ms, times out -> holds slot 0
  ASSERT_TRUE(held.has_value()) << held.error().message();
  EXPECT_EQ(held->fb_id, fb0) << "deadline miss must hold the last good slot, not advance";
  EXPECT_FALSE(held->acquire_fence.has_value()) << "fence pre-waited, never handed to the kernel";

  // Recover: the fence signals (a byte makes the pipe read-ready), and the next
  // submit's fence now passes the deadline wait, so acquire advances to slot 1.
  ASSERT_EQ(::write(pipefd[1], "x", 1), 1);
  auto signaled = drm::sync::SyncFence::import_fd(pipefd[0]);
  ASSERT_TRUE(signaled.has_value());
  r.submit(1, std::move(*signaled));
  auto recovered = r.acquire();
  ASSERT_TRUE(recovered.has_value()) << recovered.error().message();
  EXPECT_NE(recovered->fb_id, fb0) << "a signaled fence within the deadline must advance";

  r.release(std::move(*a0));
  r.release(std::move(*held));
  r.release(std::move(*recovered));
  ::close(pipefd[0]);
  ::close(pipefd[1]);
}

// When the ring drops its own frame on a fence-deadline miss, that frame's dirty
// regions were never presented, so they must be unioned into the next presented
// frame — otherwise it under-reports relative to the dropped one. A whole-frame
// drop (or an over-cap union) collapses the carry to whole-frame.
TEST(ExternalDmaBufRingVkms, CarriesDamageAcrossFenceDeadlineDrop) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }
  drm::scene::ExternalDmaBufRing::Options opts;
  opts.fence_deadline = std::chrono::milliseconds(30);

  std::vector<std::array<drm::scene::ExternalPlaneInfo, 1>> storage;
  auto slots = slots_of(*probe, storage);
  auto ring = drm::scene::ExternalDmaBufRing::create(
      probe->dev, k_w, k_h, DRM_FORMAT_XRGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()), std::move(opts));
  ASSERT_TRUE(ring.has_value()) << ring.error().message();
  auto& r = **ring;

  // Establish a live slot 0 (no fence) so a later deadline miss can hold it.
  r.submit(0);
  auto a0 = r.acquire();
  ASSERT_TRUE(a0.has_value()) << a0.error().message();

  std::array<int, 2> pipefd{-1, -1};
  ASSERT_EQ(::pipe(pipefd.data()), 0);

  // Drop: submit slot 1 with damage dA (2 rects) behind a never-signaling fence.
  const std::array<drm::scene::DamageRect, 2> d_a{drm::scene::DamageRect{1, 1, 2, 2},
                                                  drm::scene::DamageRect{3, 3, 4, 4}};
  auto unsignaled = drm::sync::SyncFence::import_fd(pipefd[0]);
  ASSERT_TRUE(unsignaled.has_value());
  r.submit(1, std::move(*unsignaled),
           drm::span<const drm::scene::DamageRect>(d_a.data(), d_a.size()));
  auto held = r.acquire();  // times out -> holds slot 0, carries dA
  ASSERT_TRUE(held.has_value()) << held.error().message();
  EXPECT_TRUE(held->damage.empty()) << "an idle/held frame reports whole-frame";

  // Recover: signal the fence and submit slot 1 with damage dB (1 rect). The next
  // presented frame must carry dA ∪ dB = 3 rects (carried first, then this frame).
  ASSERT_EQ(::write(pipefd[1], "x", 1), 1);
  const std::array<drm::scene::DamageRect, 1> d_b{drm::scene::DamageRect{5, 5, 6, 6}};
  auto signaled = drm::sync::SyncFence::import_fd(pipefd[0]);
  ASSERT_TRUE(signaled.has_value());
  r.submit(1, std::move(*signaled),
           drm::span<const drm::scene::DamageRect>(d_b.data(), d_b.size()));
  auto adv = r.acquire();
  ASSERT_TRUE(adv.has_value()) << adv.error().message();
  ASSERT_EQ(adv->damage.size(), 3U) << "dropped frame's damage must be unioned in";
  EXPECT_EQ(adv->damage[0].x, 1);  // dA[0]
  EXPECT_EQ(adv->damage[1].x, 3);  // dA[1]
  EXPECT_EQ(adv->damage[2].x, 5);  // dB[0]

  // Carry is cleared after consumption: a plain fresh frame reports only its own.
  r.submit(0, std::nullopt, drm::span<const drm::scene::DamageRect>(d_b.data(), d_b.size()));
  auto plain = r.acquire();
  ASSERT_TRUE(plain.has_value()) << plain.error().message();
  EXPECT_EQ(plain->damage.size(), 1U) << "carry must not persist past one frame";

  r.release(std::move(*a0));
  r.release(std::move(*held));
  r.release(std::move(*adv));
  r.release(std::move(*plain));
  ::close(pipefd[0]);
  ::close(pipefd[1]);
}

// A dropped whole-frame frame (empty damage) forces the next presented frame to
// whole-frame, since the ring can't know which regions the drop dirtied.
TEST(ExternalDmaBufRingVkms, DroppedWholeFrameForcesWholeFrameCarry) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card whose PRIME fd imports as a KMS FB";
  }
  drm::scene::ExternalDmaBufRing::Options opts;
  opts.fence_deadline = std::chrono::milliseconds(30);

  std::vector<std::array<drm::scene::ExternalPlaneInfo, 1>> storage;
  auto slots = slots_of(*probe, storage);
  auto ring = drm::scene::ExternalDmaBufRing::create(
      probe->dev, k_w, k_h, DRM_FORMAT_XRGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()), std::move(opts));
  ASSERT_TRUE(ring.has_value()) << ring.error().message();
  auto& r = **ring;

  r.submit(0);
  auto a0 = r.acquire();
  ASSERT_TRUE(a0.has_value()) << a0.error().message();

  std::array<int, 2> pipefd{-1, -1};
  ASSERT_EQ(::pipe(pipefd.data()), 0);

  // Drop a whole-frame frame (no damage) behind a never-signaling fence.
  auto unsignaled = drm::sync::SyncFence::import_fd(pipefd[0]);
  ASSERT_TRUE(unsignaled.has_value());
  r.submit(1, std::move(*unsignaled));
  auto held = r.acquire();
  ASSERT_TRUE(held.has_value()) << held.error().message();

  // Recover with a small per-frame damage; the carried whole-frame dominates, so
  // the presented frame is whole-frame (empty) despite this frame's 1 rect.
  ASSERT_EQ(::write(pipefd[1], "x", 1), 1);
  const std::array<drm::scene::DamageRect, 1> d_b{drm::scene::DamageRect{5, 5, 6, 6}};
  auto signaled = drm::sync::SyncFence::import_fd(pipefd[0]);
  ASSERT_TRUE(signaled.has_value());
  r.submit(1, std::move(*signaled),
           drm::span<const drm::scene::DamageRect>(d_b.data(), d_b.size()));
  auto adv = r.acquire();
  ASSERT_TRUE(adv.has_value()) << adv.error().message();
  EXPECT_TRUE(adv->damage.empty()) << "a dropped whole-frame frame forces whole-frame";

  r.release(std::move(*a0));
  r.release(std::move(*held));
  r.release(std::move(*adv));
  ::close(pipefd[0]);
  ::close(pipefd[1]);
}

// Output discovery: ScanoutTarget::discover() is the one-call output-selection API the harness
// and real KMS share. On a connected card it returns a usable crtc + connector +
// mode + primary plane; self-skips when nothing is hooked up (headless CI).
TEST(ExternalDmaBufRingVkms, DiscoverFindsOutput) {
  auto probe = find_usable_card();
  if (!probe) {
    GTEST_SKIP() << "no dumb+modeset card";
  }
  auto target = drm::display::ScanoutTarget::discover(probe->dev);
  if (!target) {
    GTEST_SKIP() << "no connected output: " << target.error().message();
  }
  EXPECT_NE(target->connector_id, 0U);
  EXPECT_NE(target->crtc_id, 0U);
  EXPECT_NE(target->primary_plane_id, 0U);
  EXPECT_GT(target->mode.hdisplay, 0U);
}
