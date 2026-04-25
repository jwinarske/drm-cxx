// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene types. Covers the contract visible without
// a live KMS device:
//   - LayerHandle value semantics + std::hash for map keying.
//   - DisplayParams::needs_scaling() logic.
//   - CommitReport default-construction.
//   - LayerScene::create input validation (crtc_id / connector_id).
//   - LayerScene::create failure on a non-DRM fd (registry enumeration).
//   - DumbBufferSource factory failure on an invalid device.
//
// Everything that exercises the allocator / atomic-commit / modeset
// paths belongs in a VKMS integration test (see test_capture_vkms for
// the pattern) — refactoring drm::planes::Allocator and
// drm::AtomicRequest into virtual interfaces to support in-process
// mocks would be a much larger change than this milestone warrants.

#include "core/device.hpp"

#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/composite_canvas.hpp>
#include <drm-cxx/scene/display_params.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/gbm_buffer_source.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace {

drmModeModeInfo dummy_mode() {
  drmModeModeInfo m{};
  m.hdisplay = 1920;
  m.vdisplay = 1080;
  m.vrefresh = 60;
  // The kernel blob copy is byte-for-byte, so zeroed fields are fine
  // for the tests that never reach createPropertyBlob.
  return m;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// LayerHandle
// ─────────────────────────────────────────────────────────────────────

TEST(SceneLayerHandle, DefaultIsInvalid) {
  drm::scene::LayerHandle h{};
  EXPECT_FALSE(h.valid());
  EXPECT_EQ(h.id, 0U);
  EXPECT_EQ(h.generation, 0U);
}

TEST(SceneLayerHandle, NonZeroIdIsValid) {
  drm::scene::LayerHandle h{1, 0};
  EXPECT_TRUE(h.valid());
}

TEST(SceneLayerHandle, EqualityRequiresBothFields) {
  drm::scene::LayerHandle a{1, 0};
  drm::scene::LayerHandle b{1, 0};
  drm::scene::LayerHandle c{1, 1};  // same id, different generation
  drm::scene::LayerHandle d{2, 0};  // different id
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(a, d);
}

TEST(SceneLayerHandle, UsableAsUnorderedMapKey) {
  std::unordered_map<drm::scene::LayerHandle, int> m;
  m[{1, 0}] = 10;
  m[{2, 0}] = 20;
  m[{1, 1}] = 11;  // id-collision, different generation; distinct key
  EXPECT_EQ(m.size(), 3U);
  EXPECT_EQ((m.at({1, 0})), 10);
  EXPECT_EQ((m.at({1, 1})), 11);
}

TEST(SceneLayerHandle, HashDistinguishesIdFromGeneration) {
  // The hash mixes id into the upper 32 bits and generation into the
  // lower; identical (id, gen) swaps should still hash distinctly.
  std::hash<drm::scene::LayerHandle> h;
  EXPECT_NE(h({1, 0}), h({0, 1}));
  EXPECT_NE(h({1, 2}), h({2, 1}));
}

// ─────────────────────────────────────────────────────────────────────
// DisplayParams
// ─────────────────────────────────────────────────────────────────────

TEST(SceneDisplayParams, NeedsScalingWhenDstDiffersFromSrc) {
  drm::scene::DisplayParams dp;
  dp.src_rect = drm::scene::Rect{0, 0, 800, 600};
  dp.dst_rect = drm::scene::Rect{0, 0, 800, 600};
  EXPECT_FALSE(dp.needs_scaling());

  dp.dst_rect = drm::scene::Rect{0, 0, 1600, 1200};
  EXPECT_TRUE(dp.needs_scaling());

  dp.dst_rect = drm::scene::Rect{0, 0, 800, 1200};  // only height differs
  EXPECT_TRUE(dp.needs_scaling());
}

TEST(SceneDisplayParams, TranslationAloneDoesNotImplyScaling) {
  // KMS supports translation (CRTC_X/Y) without scaling — only the
  // width/height difference triggers plane-scaling requirements.
  drm::scene::DisplayParams dp;
  dp.src_rect = drm::scene::Rect{0, 0, 800, 600};
  dp.dst_rect = drm::scene::Rect{400, 300, 800, 600};
  EXPECT_FALSE(dp.needs_scaling());
}

// ─────────────────────────────────────────────────────────────────────
// CommitReport & CompositeCanvasConfig
// ─────────────────────────────────────────────────────────────────────

TEST(SceneCommitReport, DefaultsToAllZeros) {
  drm::scene::CommitReport r;
  EXPECT_EQ(r.layers_total, 0U);
  EXPECT_EQ(r.layers_assigned, 0U);
  EXPECT_EQ(r.layers_unassigned, 0U);
  EXPECT_EQ(r.composition_buckets, 0U);
  EXPECT_EQ(r.properties_written, 0U);
  EXPECT_EQ(r.fbs_attached, 0U);
  EXPECT_EQ(r.test_commits_issued, 0U);
}

TEST(SceneCompositeCanvasConfig, SaneDefaults) {
  drm::scene::CompositeCanvasConfig cc;
  // Default pool size: enough for one bucket per likely plane count
  // without over-allocating. Specific number is a tuning choice — the
  // invariant is that it's > 0.
  EXPECT_GT(cc.max_canvases, 0U);
  // Width/height default to zero; scene fills them from the CRTC mode.
  EXPECT_EQ(cc.canvas_width, 0U);
  EXPECT_EQ(cc.canvas_height, 0U);
}

// ─────────────────────────────────────────────────────────────────────
// LayerScene::create input validation
// ─────────────────────────────────────────────────────────────────────

TEST(SceneLayerScene, CreateRejectsZeroCrtcId) {
  auto dev = drm::Device::from_fd(-1);
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = 0;
  cfg.connector_id = 1;
  cfg.mode = dummy_mode();
  auto r = drm::scene::LayerScene::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneLayerScene, CreateRejectsZeroConnectorId) {
  auto dev = drm::Device::from_fd(-1);
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = 1;
  cfg.connector_id = 0;
  cfg.mode = dummy_mode();
  auto r = drm::scene::LayerScene::create(dev, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneLayerScene, CreateFailsGracefullyOnInvalidDevice) {
  // Valid ids, but the Device has no fd — PlaneRegistry::enumerate
  // returns an error which create() propagates.
  auto dev = drm::Device::from_fd(-1);
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = 1;
  cfg.connector_id = 2;
  cfg.mode = dummy_mode();
  auto r = drm::scene::LayerScene::create(dev, cfg);
  EXPECT_FALSE(r.has_value());
}

TEST(SceneLayerScene, CreateFailsGracefullyOnNonDrmFd) {
  const int fd = ::open("/dev/null", O_RDWR);
  if (fd < 0) {
    GTEST_SKIP() << "Cannot open /dev/null";
  }
  auto dev = drm::Device::from_fd(fd);
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = 1;
  cfg.connector_id = 2;
  cfg.mode = dummy_mode();
  auto r = drm::scene::LayerScene::create(dev, cfg);
  EXPECT_FALSE(r.has_value());
  ::close(fd);
}

// ─────────────────────────────────────────────────────────────────────
// DumbBufferSource
// ─────────────────────────────────────────────────────────────────────

TEST(SceneDumbBufferSource, CreateFailsOnInvalidFd) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::DumbBufferSource::create(dev, 64, 64, 0x34325241U /*ARGB8888*/);
  EXPECT_FALSE(r.has_value());
}

TEST(SceneDumbBufferSource, CreateFailsOnZeroDimensions) {
  auto dev = drm::Device::from_fd(-1);
  auto r0w = drm::scene::DumbBufferSource::create(dev, 0, 64, 0x34325241U);
  EXPECT_FALSE(r0w.has_value());
  auto r0h = drm::scene::DumbBufferSource::create(dev, 64, 0, 0x34325241U);
  EXPECT_FALSE(r0h.has_value());
}

// ─────────────────────────────────────────────────────────────────────
// GbmBufferSource
// ─────────────────────────────────────────────────────────────────────

TEST(SceneGbmBufferSource, CreateFailsOnInvalidFd) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::GbmBufferSource::create(dev, 64, 64, 0x34325241U /*ARGB8888*/);
  EXPECT_FALSE(r.has_value());
}

TEST(SceneGbmBufferSource, CreateFailsOnZeroDimensions) {
  // Open a real DRM device so GbmDevice::create can succeed and the
  // zero-dimension rejection comes from Buffer::create itself.
  const int fd = ::open("/dev/dri/card0", O_RDWR);
  if (fd < 0) {
    GTEST_SKIP() << "No DRM device available for GBM";
  }
  auto dev = drm::Device::from_fd(fd);
  auto r0w = drm::scene::GbmBufferSource::create(dev, 0, 64, 0x34325241U);
  EXPECT_FALSE(r0w.has_value());
  auto r0h = drm::scene::GbmBufferSource::create(dev, 64, 0, 0x34325241U);
  EXPECT_FALSE(r0h.has_value());
  ::close(fd);
}
