// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Scene-level integration test for the release-fence path: an
// ExternalDmaBufRing driven as a LayerScene layer across several real commits,
// asserting the OUT_FENCE of the commit that displaces a slot is handed back to
// the producer via the on_release callback. Exercises the whole chain — do_commit
// requesting an internal OUT_FENCE because a source wants_release_fence, the
// per-buffer stamp in finalize_frame, the triple-deferred release rotation, and
// release_with_fence delivering a valid fence keyed by acquisition token.
//
// Drives a real LayerScene commit, so it needs a modeset card with a connected
// output and DRM master: vkms on the host/CI (a deterministic virtual output),
// or real hardware (e.g. vc4 on a Raspberry Pi) via find_scene_card()'s
// fallback. Self-skips when neither is present. Buffers are mode-sized — vkms's
// primary plane has no scaler (SRC != CRTC => ERANGE), and sizing to the mode is
// the safe choice on real planes too.

#include "core/device.hpp"

#include <drm-cxx/display/scanout_target.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>  // DamageRect
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_ring.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using drm::Device;
using drm::scene::ExternalDmaBufRing;
using drm::scene::ExternalPlaneInfo;
using drm::scene::ExternalSlotDesc;
using drm::scene::LayerDesc;
using drm::scene::LayerScene;

namespace {

std::optional<std::string> find_vkms_node() {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
    const auto& p = entry.path();
    if (p.filename().string().rfind("card", 0) != 0) {
      continue;
    }
    const int fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    const bool is_vkms =
        (v != nullptr) && (v->name != nullptr) && (std::strcmp(v->name, "vkms") == 0);
    if (v != nullptr) {
      drmFreeVersion(v);
    }
    ::close(fd);
    if (is_vkms) {
      return p.string();
    }
  }
  return std::nullopt;
}

// Pick a card to drive a real scene commit through the common LayerScene path.
// Prefer vkms (host/CI: a deterministic, headless virtual output); else the
// first card whose discover() yields a connected output we can drive on real
// hardware (e.g. vc4 on a Raspberry Pi). Requires DRM master at commit time.
std::optional<std::string> find_scene_card() {
  // Hardware-validation override: target a specific node (e.g.
  // DRM_CXX_TEST_CARD=/dev/dri/card1 to drive a real GPU on a host that also has
  // vkms loaded, where the vkms preference below would otherwise win). Honored
  // first so a deliberate hardware run is never shadowed by the virtual output.
  if (const char* node = std::getenv("DRM_CXX_TEST_CARD"); node != nullptr && *node != '\0') {
    return std::string(node);
  }
  if (auto vkms = find_vkms_node()) {
    return vkms;
  }
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
    const auto& p = entry.path();
    if (p.filename().string().rfind("card", 0) != 0) {
      continue;
    }
    auto dev = Device::open(p.string());
    if (!dev) {
      continue;
    }
    if (auto r = dev->enable_universal_planes(); !r) {
      continue;
    }
    if (auto r = dev->enable_atomic(); !r) {
      continue;
    }
    if (drm::display::ScanoutTarget::discover(*dev)) {
      return p.string();  // a connected, drivable modeset output
    }
  }
  return std::nullopt;
}

// A mode-sized dumb buffer plus its PRIME fd (owned; closed on destruction).
struct Slot {
  drm::dumb::Buffer buf;
  int fd{-1};
};

}  // namespace

TEST(ExternalDmaBufRingSceneVkms, ReleaseFenceDeliveredOnDisplace) {
  const auto node = find_scene_card();
  if (!node) {
    GTEST_SKIP() << "no vkms or connected modeset card to drive a scene commit";
  }
  auto dev_r = Device::open(*node);
  ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
  auto dev = std::make_unique<Device>(std::move(*dev_r));
  ASSERT_TRUE(dev->enable_universal_planes().has_value());
  ASSERT_TRUE(dev->enable_atomic().has_value());

  auto target = drm::display::ScanoutTarget::discover(*dev);
  if (!target) {
    GTEST_SKIP() << "no connected vkms output: " << target.error().message();
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;

  // Two mode-sized dumb buffers, each exported via PRIME.
  constexpr std::size_t k_slots = 2;
  std::vector<Slot> slots_storage;
  std::vector<std::array<ExternalPlaneInfo, 1>> plane_storage;
  std::vector<ExternalSlotDesc> slot_descs;
  for (std::size_t i = 0; i < k_slots; ++i) {
    drm::dumb::Config cfg;
    cfg.width = w;
    cfg.height = h;
    cfg.drm_format = DRM_FORMAT_XRGB8888;
    cfg.bpp = 32;
    cfg.add_fb = false;
    auto buf = drm::dumb::Buffer::create(*dev, cfg);
    ASSERT_TRUE(buf.has_value()) << buf.error().message();
    int fd = -1;
    ASSERT_EQ(drmPrimeHandleToFD(dev->fd(), buf->handle(), O_CLOEXEC | O_RDWR, &fd), 0);
    ASSERT_GE(fd, 0);
    slots_storage.push_back(Slot{std::move(*buf), fd});
  }
  plane_storage.reserve(slots_storage.size());
  for (auto& s : slots_storage) {
    plane_storage.push_back({ExternalPlaneInfo{s.fd, 0, s.buf.stride()}});
  }
  slot_descs.reserve(plane_storage.size());
  for (auto& planes : plane_storage) {
    slot_descs.push_back(ExternalSlotDesc{
        DRM_FORMAT_MOD_LINEAR, drm::span<const ExternalPlaneInfo>(planes.data(), planes.size())});
  }

  int valid_fence_releases = 0;
  int total_releases = 0;
  ExternalDmaBufRing::Options opts;
  opts.on_release = [&](std::size_t /*slot*/, std::optional<drm::sync::SyncFence> fence) {
    ++total_releases;
    if (fence.has_value() && fence->valid()) {
      ++valid_fence_releases;
    }
  };
  auto ring_r = ExternalDmaBufRing::create(
      *dev, w, h, DRM_FORMAT_XRGB8888,
      drm::span<const ExternalSlotDesc>(slot_descs.data(), slot_descs.size()), std::move(opts));
  ASSERT_TRUE(ring_r.has_value()) << ring_r.error().message();
  ExternalDmaBufRing* ring = ring_r->get();  // borrow; ownership moves into the scene

  LayerDesc desc;
  desc.source = std::move(*ring_r);
  desc.display.src_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.zpos = 1;

  LayerScene::Config cfg;
  cfg.crtc_id = target->crtc_id;
  cfg.connector_id = target->connector_id;
  cfg.mode = target->mode;
  auto scene_r = LayerScene::create(*dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto scene = std::move(*scene_r);
  ASSERT_TRUE(scene->add_layer(std::move(desc)).has_value());

  // Drive several frames, alternating the submitted slot. By the third commit the
  // first frame's buffer has been displaced and retires carrying commit 2's
  // OUT_FENCE; subsequent frames keep delivering release fences.
  for (std::size_t frame = 0; frame < 5; ++frame) {
    ring->submit(frame % k_slots);
    auto c = scene->commit();
    ASSERT_TRUE(c.has_value()) << "frame " << frame << ": " << c.error().message();
  }

  EXPECT_GT(total_releases, 0) << "expected the triple-deferred ring to retire buffers";
  EXPECT_GT(valid_fence_releases, 0)
      << "expected at least one release to carry the displacing commit's OUT_FENCE";

  drmModeSetCrtc(dev->fd(), target->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}

// Per-layer hold: a ring whose producer goes idle holds its
// plane (the scene keeps committing the held FB, never blanks it) and the held
// buffer is NOT released while it is still on screen — only once a fresh submit
// supersedes it. Validates the token-keyed release through real scene commits.
TEST(ExternalDmaBufRingSceneVkms, IdleHoldsThenReleasesOnResume) {
  const auto node = find_scene_card();
  if (!node) {
    GTEST_SKIP() << "no vkms or connected modeset card to drive a scene commit";
  }
  auto dev_r = Device::open(*node);
  ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
  auto dev = std::make_unique<Device>(std::move(*dev_r));
  ASSERT_TRUE(dev->enable_universal_planes().has_value());
  ASSERT_TRUE(dev->enable_atomic().has_value());

  auto target = drm::display::ScanoutTarget::discover(*dev);
  if (!target) {
    GTEST_SKIP() << "no connected vkms output: " << target.error().message();
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;

  constexpr std::size_t k_slots = 2;
  std::vector<Slot> slots_storage;
  std::vector<std::array<ExternalPlaneInfo, 1>> plane_storage;
  std::vector<ExternalSlotDesc> slot_descs;
  for (std::size_t i = 0; i < k_slots; ++i) {
    drm::dumb::Config cfg;
    cfg.width = w;
    cfg.height = h;
    cfg.drm_format = DRM_FORMAT_XRGB8888;
    cfg.bpp = 32;
    cfg.add_fb = false;
    auto buf = drm::dumb::Buffer::create(*dev, cfg);
    ASSERT_TRUE(buf.has_value()) << buf.error().message();
    int fd = -1;
    ASSERT_EQ(drmPrimeHandleToFD(dev->fd(), buf->handle(), O_CLOEXEC | O_RDWR, &fd), 0);
    ASSERT_GE(fd, 0);
    slots_storage.push_back(Slot{std::move(*buf), fd});
  }
  plane_storage.reserve(slots_storage.size());
  for (auto& s : slots_storage) {
    plane_storage.push_back({ExternalPlaneInfo{s.fd, 0, s.buf.stride()}});
  }
  slot_descs.reserve(plane_storage.size());
  for (auto& planes : plane_storage) {
    slot_descs.push_back(ExternalSlotDesc{
        DRM_FORMAT_MOD_LINEAR, drm::span<const ExternalPlaneInfo>(planes.data(), planes.size())});
  }

  int releases = 0;
  ExternalDmaBufRing::Options opts;
  opts.on_release = [&](std::size_t /*slot*/, std::optional<drm::sync::SyncFence> /*fence*/) {
    ++releases;
  };
  auto ring_r = ExternalDmaBufRing::create(
      *dev, w, h, DRM_FORMAT_XRGB8888,
      drm::span<const ExternalSlotDesc>(slot_descs.data(), slot_descs.size()), std::move(opts));
  ASSERT_TRUE(ring_r.has_value()) << ring_r.error().message();
  ExternalDmaBufRing* ring = ring_r->get();

  LayerDesc desc;
  desc.source = std::move(*ring_r);
  desc.display.src_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.zpos = 1;

  LayerScene::Config cfg;
  cfg.crtc_id = target->crtc_id;
  cfg.connector_id = target->connector_id;
  cfg.mode = target->mode;
  auto scene_r = LayerScene::create(*dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto scene = std::move(*scene_r);
  ASSERT_TRUE(scene->add_layer(std::move(desc)).has_value());

  // One submit, then idle: the producer renders frame 0 and goes quiet.
  ring->submit(0);
  for (int frame = 0; frame < 4; ++frame) {
    auto c = scene->commit();
    ASSERT_TRUE(c.has_value()) << "idle frame " << frame << ": " << c.error().message();
    // Held, never blanked: the ring layer keeps reaching scanout.
    EXPECT_GE(c->layers_assigned, 1U)
        << "ring layer must hold its plane while idle (frame " << frame << ")";
  }
  // The held buffer is still on screen across all idle frames — releasing it
  // would let the producer overwrite a live scanout buffer.
  EXPECT_EQ(releases, 0) << "held buffer must not be released while still displayed";

  // Producer resumes: a fresh slot supersedes slot 0, which now frees exactly
  // once despite having been (re-)committed across several idle frames.
  ring->submit(1);
  for (int frame = 0; frame < 3; ++frame) {
    auto c = scene->commit();
    ASSERT_TRUE(c.has_value()) << "resume frame " << frame << ": " << c.error().message();
    ring->submit(frame % 2 == 0 ? 0 : 1);
  }
  EXPECT_GT(releases, 0) << "superseded held slot must free once the producer resumes";

  drmModeSetCrtc(dev->fd(), target->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}

// All-idle whole-commit Skip (FrameEconomy): LayerScene::content_changed() is
// true exactly when a commit would change scanout (a fresh submit, a dirty
// layer, or a structural change), so a present loop gated on it issues NO commit
// while every source is idle — the power win. Validated against a real scene.
TEST(ExternalDmaBufRingSceneVkms, ContentChangedGatesAllIdleSkip) {
  const auto node = find_scene_card();
  if (!node) {
    GTEST_SKIP() << "no vkms or connected modeset card to drive a scene commit";
  }
  auto dev_r = Device::open(*node);
  ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
  auto dev = std::make_unique<Device>(std::move(*dev_r));
  ASSERT_TRUE(dev->enable_universal_planes().has_value());
  ASSERT_TRUE(dev->enable_atomic().has_value());

  auto target = drm::display::ScanoutTarget::discover(*dev);
  if (!target) {
    GTEST_SKIP() << "no connected vkms output: " << target.error().message();
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;

  constexpr std::size_t k_slots = 2;
  std::vector<Slot> slots_storage;
  std::vector<std::array<ExternalPlaneInfo, 1>> plane_storage;
  std::vector<ExternalSlotDesc> slot_descs;
  for (std::size_t i = 0; i < k_slots; ++i) {
    drm::dumb::Config dcfg;
    dcfg.width = w;
    dcfg.height = h;
    dcfg.drm_format = DRM_FORMAT_XRGB8888;
    dcfg.bpp = 32;
    dcfg.add_fb = false;
    auto buf = drm::dumb::Buffer::create(*dev, dcfg);
    ASSERT_TRUE(buf.has_value()) << buf.error().message();
    int fd = -1;
    ASSERT_EQ(drmPrimeHandleToFD(dev->fd(), buf->handle(), O_CLOEXEC | O_RDWR, &fd), 0);
    ASSERT_GE(fd, 0);
    slots_storage.push_back(Slot{std::move(*buf), fd});
  }
  plane_storage.reserve(slots_storage.size());
  for (auto& s : slots_storage) {
    plane_storage.push_back({ExternalPlaneInfo{s.fd, 0, s.buf.stride()}});
  }
  slot_descs.reserve(plane_storage.size());
  for (auto& planes : plane_storage) {
    slot_descs.push_back(ExternalSlotDesc{
        DRM_FORMAT_MOD_LINEAR, drm::span<const ExternalPlaneInfo>(planes.data(), planes.size())});
  }

  auto ring_r = ExternalDmaBufRing::create(
      *dev, w, h, DRM_FORMAT_XRGB8888,
      drm::span<const ExternalSlotDesc>(slot_descs.data(), slot_descs.size()));
  ASSERT_TRUE(ring_r.has_value()) << ring_r.error().message();
  ExternalDmaBufRing* ring = ring_r->get();

  LayerDesc desc;
  desc.source = std::move(*ring_r);
  desc.display.src_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.zpos = 1;

  LayerScene::Config cfg;
  cfg.crtc_id = target->crtc_id;
  cfg.connector_id = target->connector_id;
  cfg.mode = target->mode;
  auto scene_r = LayerScene::create(*dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto scene = std::move(*scene_r);

  // Fresh scene with a new layer: structural change + a pending submit -> changed.
  ASSERT_TRUE(scene->add_layer(std::move(desc)).has_value());
  ring->submit(0);
  EXPECT_TRUE(scene->content_changed());
  ASSERT_TRUE(scene->commit().has_value());

  // Producer idle: nothing submitted, layer now clean -> no change to scan out.
  EXPECT_FALSE(scene->content_changed());

  // A present loop gated on content_changed() issues zero commits while idle.
  int commits = 0;
  int skips = 0;
  for (int frame = 0; frame < 4; ++frame) {
    if (scene->content_changed()) {
      ASSERT_TRUE(scene->commit().has_value());
      ++commits;
    } else {
      ++skips;  // the whole-commit Skip: no atomic commit this vblank
    }
  }
  EXPECT_EQ(commits, 0) << "an idle producer must not drive any commit";
  EXPECT_EQ(skips, 4);

  // A fresh submit flips the gate back on.
  ring->submit(1);
  EXPECT_TRUE(scene->content_changed());
  ASSERT_TRUE(scene->commit().has_value());
  EXPECT_FALSE(scene->content_changed());

  drmModeSetCrtc(dev->fd(), target->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}

// Multi-layer: a ring layer alongside a second (dumb) layer drives the common
// commit path with more than one in-flight acquisition. Covers release-fence
// stamping over multiple prev acquisitions, content_changed() across layers (a
// default-fresh dumb source keeps the scene changed even when the ring is idle),
// and the structural-dirty path: removing a layer must force a commit so its
// plane is disabled rather than idle-skipped.
TEST(ExternalDmaBufRingSceneVkms, MultiLayerReleaseFenceAndRemove) {
  const auto node = find_scene_card();
  if (!node) {
    GTEST_SKIP() << "no vkms or connected modeset card to drive a scene commit";
  }
  auto dev_r = Device::open(*node);
  ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
  auto dev = std::make_unique<Device>(std::move(*dev_r));
  ASSERT_TRUE(dev->enable_universal_planes().has_value());
  ASSERT_TRUE(dev->enable_atomic().has_value());

  auto target = drm::display::ScanoutTarget::discover(*dev);
  if (!target) {
    GTEST_SKIP() << "no connected output: " << target.error().message();
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;

  // Ring layer (mode-sized, on PRIMARY), with an on_release that counts the
  // OUT_FENCE-carrying releases.
  constexpr std::size_t k_slots = 2;
  std::vector<Slot> slots_storage;
  std::vector<std::array<ExternalPlaneInfo, 1>> plane_storage;
  std::vector<ExternalSlotDesc> slot_descs;
  for (std::size_t i = 0; i < k_slots; ++i) {
    drm::dumb::Config dcfg;
    dcfg.width = w;
    dcfg.height = h;
    dcfg.drm_format = DRM_FORMAT_XRGB8888;
    dcfg.bpp = 32;
    dcfg.add_fb = false;
    auto buf = drm::dumb::Buffer::create(*dev, dcfg);
    ASSERT_TRUE(buf.has_value()) << buf.error().message();
    int fd = -1;
    ASSERT_EQ(drmPrimeHandleToFD(dev->fd(), buf->handle(), O_CLOEXEC | O_RDWR, &fd), 0);
    ASSERT_GE(fd, 0);
    slots_storage.push_back(Slot{std::move(*buf), fd});
  }
  plane_storage.reserve(slots_storage.size());
  for (auto& s : slots_storage) {
    plane_storage.push_back({ExternalPlaneInfo{s.fd, 0, s.buf.stride()}});
  }
  slot_descs.reserve(plane_storage.size());
  for (auto& planes : plane_storage) {
    slot_descs.push_back(ExternalSlotDesc{
        DRM_FORMAT_MOD_LINEAR, drm::span<const ExternalPlaneInfo>(planes.data(), planes.size())});
  }

  int valid_fence_releases = 0;
  ExternalDmaBufRing::Options opts;
  opts.on_release = [&](std::size_t /*slot*/, std::optional<drm::sync::SyncFence> fence) {
    if (fence.has_value() && fence->valid()) {
      ++valid_fence_releases;
    }
  };
  auto ring_r = ExternalDmaBufRing::create(
      *dev, w, h, DRM_FORMAT_XRGB8888,
      drm::span<const ExternalSlotDesc>(slot_descs.data(), slot_descs.size()), std::move(opts));
  ASSERT_TRUE(ring_r.has_value()) << ring_r.error().message();
  ExternalDmaBufRing* ring = ring_r->get();

  LayerScene::Config cfg;
  cfg.crtc_id = target->crtc_id;
  cfg.connector_id = target->connector_id;
  cfg.mode = target->mode;
  auto scene_r = LayerScene::create(*dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto scene = std::move(*scene_r);

  LayerDesc ring_desc;
  ring_desc.source = std::move(*ring_r);
  ring_desc.display.src_rect = drm::scene::Rect{0, 0, w, h};
  ring_desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  ring_desc.display.zpos = 1;
  ASSERT_TRUE(scene->add_layer(std::move(ring_desc)).has_value());

  // Second layer: a quarter-screen dumb-buffer overlay on top.
  const std::uint32_t ow = w / 2;
  const std::uint32_t oh = h / 2;
  auto dumb_src = drm::scene::DumbBufferSource::create(*dev, ow, oh, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(dumb_src.has_value()) << dumb_src.error().message();
  LayerDesc dumb_desc;
  dumb_desc.source = std::move(*dumb_src);
  dumb_desc.display.src_rect = drm::scene::Rect{0, 0, ow, oh};
  dumb_desc.display.dst_rect = drm::scene::Rect{0, 0, ow, oh};
  // zpos >= 3: amdgpu DC pins its PRIMARY plane at zpos 2, so an explicit
  // overlay zpos <= 2 collides with it and the multi-plane commit EINVALs on
  // real amdgpu (vkms is permissive and accepts any zpos). 3 is portable to both.
  dumb_desc.display.zpos = 3;
  auto dumb_handle = scene->add_layer(std::move(dumb_desc));
  ASSERT_TRUE(dumb_handle.has_value()) << dumb_handle.error().message();

  // Drive frames with both layers live: the ring submits each frame, the dumb
  // layer is static. Two acquisitions per commit exercise the release-fence
  // stamping loop over multiple prev acquisitions.
  ring->submit(0);
  auto first = scene->commit();
  ASSERT_TRUE(first.has_value()) << first.error().message();
  EXPECT_EQ(first->layers_total, 2U);

  for (int frame = 1; frame < 5; ++frame) {
    ring->submit(static_cast<std::size_t>(frame) % k_slots);
    // A default-fresh dumb source keeps the scene changed regardless of the ring.
    EXPECT_TRUE(scene->content_changed());
    auto c = scene->commit();
    ASSERT_TRUE(c.has_value()) << "frame " << frame << ": " << c.error().message();
  }
  EXPECT_GT(valid_fence_releases, 0)
      << "release fences must be delivered with a second layer also in flight";

  // The dumb source is default-fresh, so the scene stays changed even with the
  // ring idle — the all-idle skip cannot fire while a non-idle-aware layer is up.
  EXPECT_TRUE(scene->content_changed());

  // Removing a layer is a structural change: the next commit must run to disable
  // its plane, never be idle-skipped.
  scene->remove_layer(*dumb_handle);
  EXPECT_TRUE(scene->content_changed()) << "remove_layer must force a commit";
  auto post_remove = scene->commit();
  ASSERT_TRUE(post_remove.has_value()) << "post-remove commit: " << post_remove.error().message();

  // Only the (idle) ring remains: clean and idle -> the scene is now skippable.
  EXPECT_FALSE(scene->content_changed());

  drmModeSetCrtc(dev->fd(), target->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}

// Per-frame damage driven through real LayerScene commits: the ring reports a
// dirty sub-rect each frame, the scene emits FB_DAMAGE_CLIPS on planes that
// advertise it, and the commit must still succeed. On vkms this is a smoke test
// (the property is ignored); on amdgpu (DRM_CXX_TEST_CARD=/dev/dri/card1) it
// validates that a real driver accepts the damage blob end-to-end.
TEST(ExternalDmaBufRingSceneVkms, PerFrameDamageCommits) {
  const auto node = find_scene_card();
  if (!node) {
    GTEST_SKIP() << "no vkms or connected modeset card to drive a scene commit";
  }
  auto dev_r = Device::open(*node);
  ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
  auto dev = std::make_unique<Device>(std::move(*dev_r));
  ASSERT_TRUE(dev->enable_universal_planes().has_value());
  ASSERT_TRUE(dev->enable_atomic().has_value());

  auto target = drm::display::ScanoutTarget::discover(*dev);
  if (!target) {
    GTEST_SKIP() << "no connected output: " << target.error().message();
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;

  constexpr std::size_t k_slots = 2;
  std::vector<Slot> slots_storage;
  std::vector<std::array<ExternalPlaneInfo, 1>> plane_storage;
  std::vector<ExternalSlotDesc> slot_descs;
  for (std::size_t i = 0; i < k_slots; ++i) {
    drm::dumb::Config cfg;
    cfg.width = w;
    cfg.height = h;
    cfg.drm_format = DRM_FORMAT_XRGB8888;
    cfg.bpp = 32;
    cfg.add_fb = false;
    auto buf = drm::dumb::Buffer::create(*dev, cfg);
    ASSERT_TRUE(buf.has_value()) << buf.error().message();
    int fd = -1;
    ASSERT_EQ(drmPrimeHandleToFD(dev->fd(), buf->handle(), O_CLOEXEC | O_RDWR, &fd), 0);
    ASSERT_GE(fd, 0);
    slots_storage.push_back(Slot{std::move(*buf), fd});
  }
  plane_storage.reserve(slots_storage.size());
  for (auto& s : slots_storage) {
    plane_storage.push_back({ExternalPlaneInfo{s.fd, 0, s.buf.stride()}});
  }
  slot_descs.reserve(plane_storage.size());
  for (auto& planes : plane_storage) {
    slot_descs.push_back(ExternalSlotDesc{
        DRM_FORMAT_MOD_LINEAR, drm::span<const ExternalPlaneInfo>(planes.data(), planes.size())});
  }

  auto ring_r = ExternalDmaBufRing::create(
      *dev, w, h, DRM_FORMAT_XRGB8888,
      drm::span<const ExternalSlotDesc>(slot_descs.data(), slot_descs.size()));
  ASSERT_TRUE(ring_r.has_value()) << ring_r.error().message();
  ExternalDmaBufRing* ring = ring_r->get();

  LayerDesc desc;
  desc.source = std::move(*ring_r);
  desc.display.src_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.zpos = 1;

  LayerScene::Config cfg;
  cfg.crtc_id = target->crtc_id;
  cfg.connector_id = target->connector_id;
  cfg.mode = target->mode;
  auto scene_r = LayerScene::create(*dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto scene = std::move(*scene_r);
  ASSERT_TRUE(scene->add_layer(std::move(desc)).has_value());

  // A small dirty rect well inside the mode, varying per frame so each commit
  // carries a distinct FB_DAMAGE_CLIPS blob.
  for (std::size_t frame = 0; frame < 5; ++frame) {
    const std::array<drm::scene::DamageRect, 1> dmg{
        drm::scene::DamageRect{static_cast<std::int32_t>(frame * 8), 0, 32, 32}};
    ring->submit(frame % k_slots, std::nullopt,
                 drm::span<const drm::scene::DamageRect>(dmg.data(), dmg.size()));
    auto c = scene->commit();
    ASSERT_TRUE(c.has_value()) << "frame " << frame << " (damage commit): " << c.error().message();
  }

  drmModeSetCrtc(dev->fd(), target->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}
