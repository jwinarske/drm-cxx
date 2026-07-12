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

#include <drm-cxx/display/hdr_metadata.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/composite_canvas.hpp>
#include <drm-cxx/scene/display_params.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/gbm_buffer_source.hpp>
#include <drm-cxx/scene/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <optional>
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
  EXPECT_TRUE(r.placements.empty());
}

TEST(SceneCommitReport, LayerPlacementEntryDefaults) {
  const drm::scene::LayerPlacementEntry e;
  EXPECT_FALSE(e.handle.valid());
  EXPECT_EQ(e.placement, drm::scene::LayerPlacement::Unassigned);
  EXPECT_EQ(e.plane_id, 0U);
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

// ─────────────────────────────────────────────────────────────────────
// Layer placement readout — defaults visible without a live KMS device.
// The full assigned-/composited-/dropped-via-LayerScene round-trip is
// covered by the VKMS integration suites, where a real allocator runs
// against a writeback CRTC.
// ─────────────────────────────────────────────────────────────────────

TEST(SceneLayer, PlacementDefaultsToUnassigned) {
  // Construct a Layer directly through the public ctor (LayerScene
  // would normally mint one via add_layer; the ctor is exposed for
  // pimpl reasons and stable here).
  const drm::scene::LayerHandle h{1, 0};
  const drm::scene::DisplayParams dp;
  const drm::scene::Layer layer{h, /*source=*/nullptr, dp, drm::planes::ContentType::Generic,
                                /*update_hint_hz=*/0};
  EXPECT_EQ(layer.last_placement(), drm::scene::LayerPlacement::Unassigned);
  EXPECT_FALSE(layer.last_assigned_plane_id().has_value());
}

TEST(SceneLayer, RecordPlacementAssignedToPlane) {
  const drm::scene::LayerHandle h{1, 0};
  const drm::scene::DisplayParams dp;
  drm::scene::Layer layer{h, /*source=*/nullptr, dp, drm::planes::ContentType::Generic, 0U};
  layer.record_placement(drm::scene::LayerPlacement::AssignedToPlane, std::uint32_t{73});
  EXPECT_EQ(layer.last_placement(), drm::scene::LayerPlacement::AssignedToPlane);
  EXPECT_EQ(layer.last_assigned_plane_id().value_or(0U), 73U);
}

TEST(SceneLayer, RecordPlacementComposited) {
  const drm::scene::LayerHandle h{2, 0};
  const drm::scene::DisplayParams dp;
  drm::scene::Layer layer{h, /*source=*/nullptr, dp, drm::planes::ContentType::UI, 60U};
  layer.record_placement(drm::scene::LayerPlacement::Composited, std::uint32_t{42});
  EXPECT_EQ(layer.last_placement(), drm::scene::LayerPlacement::Composited);
  EXPECT_EQ(layer.last_assigned_plane_id().value_or(0U), 42U);
}

TEST(SceneLayer, RecordPlacementUnassignedClearsPlaneId) {
  const drm::scene::LayerHandle h{3, 0};
  const drm::scene::DisplayParams dp;
  drm::scene::Layer layer{h, /*source=*/nullptr, dp, drm::planes::ContentType::Generic, 0U};
  layer.record_placement(drm::scene::LayerPlacement::AssignedToPlane, std::uint32_t{55});
  // Subsequent commit drops the layer.
  layer.record_placement(drm::scene::LayerPlacement::Unassigned, {});
  EXPECT_EQ(layer.last_placement(), drm::scene::LayerPlacement::Unassigned);
  EXPECT_FALSE(layer.last_assigned_plane_id().has_value());
}

// ─────────────────────────────────────────────────────────────────────
// Conditional setters (Proposal 1) — `_if_changed` variants flip the
// dirty flag only when the value actually changes, so steady-state
// frames that resubmit unchanged geometry leave the layer clean.
// ─────────────────────────────────────────────────────────────────────

namespace {
// Construct a clean scene Layer for the conditional-setter tests. The
// ctor starts the layer dirty (first commit writes every property);
// mark_clean() puts it in the steady-state condition these tests probe.
drm::scene::Layer make_clean_layer(const drm::scene::DisplayParams& dp = {}) {
  const drm::scene::LayerHandle h{1, 0};
  drm::scene::Layer layer{h, /*source=*/nullptr, dp, drm::planes::ContentType::Generic, 0U};
  layer.mark_clean();
  return layer;
}
}  // namespace

TEST(SceneLayerIfChanged, DstRectDirtiesOnChangeOnly) {
  auto layer = make_clean_layer();
  ASSERT_FALSE(layer.is_dirty());

  layer.set_dst_rect_if_changed(drm::scene::Rect{0, 0, 1920, 1080});
  EXPECT_TRUE(layer.is_dirty());

  layer.mark_clean();
  // Same value: must stay clean.
  layer.set_dst_rect_if_changed(drm::scene::Rect{0, 0, 1920, 1080});
  EXPECT_FALSE(layer.is_dirty());

  // Different value: dirties.
  layer.set_dst_rect_if_changed(drm::scene::Rect{0, 0, 1280, 720});
  EXPECT_TRUE(layer.is_dirty());
}

TEST(SceneLayerIfChanged, SrcRectDirtiesOnChangeOnly) {
  auto layer = make_clean_layer();
  layer.set_src_rect_if_changed(drm::scene::Rect{0, 0, 100, 100});
  EXPECT_TRUE(layer.is_dirty());
  layer.mark_clean();
  layer.set_src_rect_if_changed(drm::scene::Rect{0, 0, 100, 100});
  EXPECT_FALSE(layer.is_dirty());
}

TEST(SceneLayerIfChanged, RotationDirtiesOnChangeOnly) {
  auto layer = make_clean_layer();
  layer.set_rotation_if_changed(1);
  EXPECT_TRUE(layer.is_dirty());
  layer.mark_clean();
  layer.set_rotation_if_changed(1);
  EXPECT_FALSE(layer.is_dirty());
}

TEST(SceneLayerIfChanged, ZposDirtiesOnChangeOnly) {
  auto layer = make_clean_layer();
  layer.set_zpos_if_changed(3);
  EXPECT_TRUE(layer.is_dirty());
  layer.mark_clean();
  layer.set_zpos_if_changed(3);
  EXPECT_FALSE(layer.is_dirty());
  // nullopt vs a set value differs.
  layer.set_zpos_if_changed(std::nullopt);
  EXPECT_TRUE(layer.is_dirty());
}

TEST(SceneLayerIfChanged, ColorPrimariesAndEotfDirtyOnChangeOnly) {
  auto layer = make_clean_layer();
  layer.set_source_eotf_if_changed(drm::display::TransferFunction::SmpteSt2084Pq);
  EXPECT_TRUE(layer.is_dirty());
  layer.mark_clean();
  // Stream resubmits the same transfer function every frame: stays clean.
  layer.set_source_eotf_if_changed(drm::display::TransferFunction::SmpteSt2084Pq);
  EXPECT_FALSE(layer.is_dirty());

  // Color primaries follow the same shape: first set dirties, identical
  // resubmit stays clean, a real change dirties again.
  layer.set_color_primaries_if_changed(drm::scene::ColorPrimaries::Bt2020);
  EXPECT_TRUE(layer.is_dirty());
  layer.mark_clean();
  layer.set_color_primaries_if_changed(drm::scene::ColorPrimaries::Bt2020);
  EXPECT_FALSE(layer.is_dirty());
  layer.set_color_primaries_if_changed(drm::scene::ColorPrimaries::Bt709);
  EXPECT_TRUE(layer.is_dirty());
}

TEST(SceneLayerIfChanged, AlphaFirstCallAlwaysDirtiesEvenAtDefault) {
  // DisplayParams::alpha defaults to 0xFFFF (opaque). The first
  // conditional set to that same default must still dirty and set the
  // sticky explicit bit, because the implicit pre-call alpha is
  // conceptually distinct from an explicitly-set value.
  auto layer = make_clean_layer();
  ASSERT_FALSE(layer.alpha_was_explicitly_set());

  layer.set_alpha_if_changed(0xFFFF);
  EXPECT_TRUE(layer.is_dirty());
  EXPECT_TRUE(layer.alpha_was_explicitly_set());

  layer.mark_clean();
  // Second call with the same value no longer dirties.
  layer.set_alpha_if_changed(0xFFFF);
  EXPECT_FALSE(layer.is_dirty());

  // A genuine change dirties; the round-trip back to opaque also dirties.
  layer.set_alpha_if_changed(0x8000);
  EXPECT_TRUE(layer.is_dirty());
  layer.mark_clean();
  layer.set_alpha_if_changed(0xFFFF);
  EXPECT_TRUE(layer.is_dirty());
}

// ─────────────────────────────────────────────────────────────────────
// identity_tag (Proposal 2) — forwarded LayerDesc field, recovered by
// LayerScene::find_by_identity_tag. The full scene-level round-trip
// (plus survival across rebind / session resume) is covered by the
// vkms integration tests; this section pins down the Layer-side
// storage contract that doesn't need a live device.
// ─────────────────────────────────────────────────────────────────────

TEST(SceneLayerIdentityTag, LayerDescDefaultsToNullptr) {
  const drm::scene::LayerDesc desc;
  EXPECT_EQ(desc.identity_tag, nullptr);
}

TEST(SceneLayerIdentityTag, LayerCtorDefaultsToNullptr) {
  const drm::scene::LayerHandle h{1, 0};
  const drm::scene::DisplayParams dp;
  // Legacy ctor call site (5 args) — exercises the default identity_tag
  // value, proving the new parameter is source-compatible.
  const drm::scene::Layer layer{h, /*source=*/nullptr, dp, drm::planes::ContentType::Generic, 0U};
  EXPECT_EQ(layer.identity_tag(), nullptr);
}

TEST(SceneLayerIdentityTag, LayerCtorStoresAndReturnsValue) {
  const drm::scene::LayerHandle h{2, 0};
  const drm::scene::DisplayParams dp;
  int sentinel = 0;
  // The scene never dereferences the tag — any non-null pointer the
  // caller hands over must round-trip verbatim.
  const drm::scene::Layer layer{h,   /*source=*/nullptr, dp,       drm::planes::ContentType::UI,
                                60U, /*app_priority=*/0, &sentinel};
  EXPECT_EQ(layer.identity_tag(), &sentinel);
}

TEST(SceneLayerAppPriority, CtorDefaultsToZero) {
  const drm::scene::LayerHandle h{1, 0};
  const drm::scene::DisplayParams dp;
  // 5-arg legacy form leaves app_priority at its default.
  const drm::scene::Layer layer{h, /*source=*/nullptr, dp, drm::planes::ContentType::Generic, 0U};
  EXPECT_EQ(layer.app_priority(), 0U);
}

TEST(SceneLayerAppPriority, CtorStoresAndReturnsValue) {
  const drm::scene::LayerHandle h{2, 0};
  const drm::scene::DisplayParams dp;
  const drm::scene::Layer layer{h,
                                /*source=*/nullptr,
                                dp,
                                drm::planes::ContentType::Video,
                                /*update_hint_hz=*/60U,
                                /*app_priority=*/200};
  EXPECT_EQ(layer.app_priority(), 200U);
}

TEST(SceneLayerAppPriority, SetAppPriorityFlagsHintsDirty) {
  auto layer = make_clean_layer();
  EXPECT_FALSE(layer.hints_dirty());
  layer.set_app_priority(5);
  EXPECT_EQ(layer.app_priority(), 5U);
  // Priority feeds plane scoring, so a change must drop the allocator
  // warm-start — same contract as set_content_type / set_update_hint.
  EXPECT_TRUE(layer.hints_dirty());
  EXPECT_TRUE(layer.is_dirty());
}

TEST(SceneLayerPin, CtorDefaultsToNoPin) {
  const drm::scene::LayerHandle h{1, 0};
  const drm::scene::DisplayParams dp;
  // 5-arg legacy form leaves the layer unpinned.
  const drm::scene::Layer layer{h, /*source=*/nullptr, dp, drm::planes::ContentType::Video, 0U};
  EXPECT_FALSE(layer.pinned_plane_id().has_value());
}

TEST(SceneLayerPin, CtorStoresPinnedPlaneId) {
  const drm::scene::LayerHandle h{2, 0};
  const drm::scene::DisplayParams dp;
  const drm::scene::Layer layer{h,
                                /*source=*/nullptr,
                                dp,
                                drm::planes::ContentType::Video,
                                /*update_hint_hz=*/60U,
                                /*app_priority=*/0,
                                /*identity_tag=*/nullptr,
                                /*pinned_plane_id=*/std::uint32_t{42}};
  ASSERT_TRUE(layer.pinned_plane_id().has_value());
  EXPECT_EQ(*layer.pinned_plane_id(), 42U);
}

TEST(SceneLayerDescPin, DefaultsToNoPin) {
  const drm::scene::LayerDesc d;
  EXPECT_FALSE(d.pinned_plane_id.has_value());
}

TEST(SceneCommitReportPin, PinsFailedDefaultsToZero) {
  const drm::scene::CommitReport r;
  EXPECT_EQ(r.pins_failed, 0U);
}
