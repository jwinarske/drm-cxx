// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// SceneSet integration tests against vkms.
//
// Requires a vkms instance with >= 2 connected virtual connectors.
// scripts/vkms_dual.sh handles the configfs provisioning:
//
//   sudo scripts/vkms_dual.sh up      # create the "dual" instance
//   sudo scripts/vkms_dual.sh down    # tear it down
//
// Tests GTEST_SKIP when no vkms instance has >=2 connected outputs.

#include "../../examples/common/multi_crtc_probe.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/display/hdr_metadata.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/display_params.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/scene_set.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include <cstdint>
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

namespace {

// Reuse the multi_crtc_probe helper for output enumeration so this
// test stays aligned with the bare-TTY validation path on amdgpu.
using drm::examples::multi_crtc::ConnectedOutput;
using drm::examples::multi_crtc::enumerate_connected_outputs;

// Locate the first vkms /dev/dri/cardN node with at least
// `min_outputs` connected virtual connectors. Returns nullopt when
// no qualifying instance is present, including when vkms isn't
// loaded at all.
std::optional<std::string> find_vkms_multi_crtc(std::size_t min_outputs) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
    const auto& p = entry.path();
    const std::string name = p.filename().string();
    if (name.rfind("card", 0) != 0) {
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
    if (!is_vkms) {
      ::close(fd);
      continue;
    }
    // Have a vkms fd; check whether its enumeration yields >= min_outputs.
    auto dev = drm::Device::from_fd(fd);
    (void)dev.enable_universal_planes();
    (void)dev.enable_atomic();
    const auto outputs = enumerate_connected_outputs(dev);
    ::close(fd);
    if (outputs.size() >= min_outputs) {
      return p.string();
    }
  }
  return std::nullopt;
}

// Build N LayerScenes against the supplied outputs. Owns the
// underlying Device too so the lifetime nesting matches LayerScene's
// captured-by-reference contract.
struct ScenesFixture {
  std::unique_ptr<drm::Device> dev;
  std::vector<ConnectedOutput> outputs;
  std::vector<std::unique_ptr<drm::scene::LayerScene>> scenes;
};

drm::expected<ScenesFixture, std::error_code> open_scenes(const std::string& node) {
  auto dev_r = drm::Device::open(node);
  if (!dev_r) {
    return drm::unexpected<std::error_code>(dev_r.error());
  }
  auto dev = std::make_unique<drm::Device>(std::move(*dev_r));
  if (auto r = dev->enable_universal_planes(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = dev->enable_atomic(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  auto outputs = enumerate_connected_outputs(*dev);
  if (outputs.size() < 2) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_device_or_address));
  }
  std::vector<std::unique_ptr<drm::scene::LayerScene>> scenes;
  scenes.reserve(outputs.size());
  for (const auto& o : outputs) {
    drm::scene::LayerScene::Config cfg;
    cfg.crtc_id = o.crtc_id;
    cfg.connector_id = o.connector_id;
    cfg.mode = o.mode;
    auto s = drm::scene::LayerScene::create(*dev, cfg);
    if (!s) {
      return drm::unexpected<std::error_code>(s.error());
    }
    scenes.push_back(std::move(*s));
  }
  return ScenesFixture{std::move(dev), std::move(outputs), std::move(scenes)};
}

}  // namespace

// Combined cross-CRTC TEST_ONLY: each scene carries its own dumb-buffer
// layer sized to its mode; SceneSet packs the two scenes' property
// writes into one drmModeAtomicCommit and asks the kernel to validate
// the combination. Acceptance proves SceneSet's build → submit → finalize
// path produces a kernel-acceptable atomic on multi-CRTC vkms.
TEST(SceneSetVkms, CombinedTestAcceptsMultiCrtc) {
  const auto node = find_vkms_multi_crtc(2);
  if (!node) {
    GTEST_SKIP() << "no vkms instance with >=2 connected outputs "
                    "(run `sudo scripts/vkms_dual.sh up`)";
  }
  auto fx_r = open_scenes(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  for (std::size_t i = 0; i < fx.outputs.size(); ++i) {
    const auto& o = fx.outputs[i];
    auto src = drm::scene::DumbBufferSource::create(*fx.dev, o.mode.hdisplay, o.mode.vdisplay,
                                                    DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << "DumbBufferSource for " << o.connector_name << ": "
                                 << src.error().message();
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    desc.display.dst_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    auto h = fx.scenes[i]->add_layer(std::move(desc));
    ASSERT_TRUE(h.has_value()) << "add_layer for " << o.connector_name << ": "
                               << h.error().message();
  }

  auto set_r = drm::scene::SceneSet::create(*fx.dev, std::move(fx.scenes));
  ASSERT_TRUE(set_r.has_value()) << set_r.error().message();

  auto reports = (*set_r)->test();
  ASSERT_TRUE(reports.has_value()) << reports.error().message();
  EXPECT_EQ(reports->size(), fx.outputs.size());
  for (std::size_t i = 0; i < reports->size(); ++i) {
    const auto& r = (*reports)[i];
    EXPECT_EQ(r.layers_total, 1U) << "scene " << i << " (" << fx.outputs[i].connector_name
                                  << ") should report exactly one layer";
  }
}

// Mirrored single shared source across N scenes via
// SceneSet::add_layer. Validates the SharedLayerBufferSource forwarder
// path under a real kernel — every participating scene calls the
// forwarder's acquire(), which round-trips to the same underlying
// DumbBufferSource, and the combined TEST commit lands.
TEST(SceneSetVkms, MirroredLayerAcceptsAcrossMultiCrtc) {
  const auto node = find_vkms_multi_crtc(2);
  if (!node) {
    GTEST_SKIP() << "no vkms instance with >=2 connected outputs";
  }
  auto fx_r = open_scenes(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  auto set_r = drm::scene::SceneSet::create(*fx.dev, std::move(fx.scenes));
  ASSERT_TRUE(set_r.has_value()) << set_r.error().message();

  constexpr std::uint32_t mirror_side = 256;
  auto shared =
      drm::scene::DumbBufferSource::create(*fx.dev, mirror_side, mirror_side, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(shared.has_value()) << shared.error().message();
  const std::shared_ptr<drm::scene::LayerBufferSource> shared_src(std::move(*shared));

  drm::scene::SceneSetLayerSpec spec;
  spec.source = shared_src;
  spec.targets.reserve(fx.outputs.size());
  for (std::size_t i = 0; i < fx.outputs.size(); ++i) {
    const auto& o = fx.outputs[i];
    const std::uint32_t cx =
        (o.mode.hdisplay > mirror_side) ? (o.mode.hdisplay - mirror_side) / 2 : 0;
    const std::uint32_t cy =
        (o.mode.vdisplay > mirror_side) ? (o.mode.vdisplay - mirror_side) / 2 : 0;
    drm::scene::DisplayParams display;
    display.src_rect = drm::scene::Rect{0, 0, mirror_side, mirror_side};
    display.dst_rect = drm::scene::Rect{static_cast<std::int32_t>(cx),
                                        static_cast<std::int32_t>(cy), mirror_side, mirror_side};
    spec.targets.push_back({.scene_index = i, .display = display, .force_composited = false});
  }

  auto handle = (*set_r)->add_layer(spec);
  ASSERT_TRUE(handle.has_value()) << handle.error().message();
  EXPECT_TRUE(handle->valid());

  auto reports = (*set_r)->test();
  ASSERT_TRUE(reports.has_value()) << reports.error().message();
  EXPECT_EQ(reports->size(), fx.outputs.size());
  for (std::size_t i = 0; i < reports->size(); ++i) {
    const auto& r = (*reports)[i];
    EXPECT_EQ(r.layers_total, 1U) << "scene " << i << " should see exactly the mirrored layer";
  }

  // Removing the handle should clear every per-scene layer, leaving
  // an empty scene that still accepts a combined test commit.
  (*set_r)->remove_layer(*handle);
  auto post_remove = (*set_r)->test();
  ASSERT_TRUE(post_remove.has_value()) << post_remove.error().message();
  for (const auto& r : *post_remove) {
    EXPECT_EQ(r.layers_total, 0U);
  }
}

// Hotplug round-trip: build a SceneSet with one of the two vkms
// outputs, then add_scene the second one, run a combined test commit,
// remove_scene the first slot, and verify the resulting hole survives
// a follow-up combined test commit. Also confirms add_scene reuses the
// hole at the lowest index rather than appending.
TEST(SceneSetVkms, AddRemoveSceneRoundTrip) {
  const auto node = find_vkms_multi_crtc(2);
  if (!node) {
    GTEST_SKIP() << "no vkms instance with >=2 connected outputs";
  }
  auto fx_r = open_scenes(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  ASSERT_GE(fx.scenes.size(), 2U);

  // Keep scene[1] on the side; build the set with just scene[0].
  std::unique_ptr<drm::scene::LayerScene> spare = std::move(fx.scenes[1]);
  fx.scenes.pop_back();
  auto set_r = drm::scene::SceneSet::create(*fx.dev, std::move(fx.scenes));
  ASSERT_TRUE(set_r.has_value()) << set_r.error().message();
  EXPECT_EQ((*set_r)->scene_count(), 1U);

  // Single-scene combined test commit still lands.
  {
    auto reports = (*set_r)->test();
    ASSERT_TRUE(reports.has_value()) << reports.error().message();
    EXPECT_EQ(reports->size(), 1U);
  }

  // add_scene appends to grow the set to two entries.
  auto added = (*set_r)->add_scene(std::move(spare));
  ASSERT_TRUE(added.has_value()) << added.error().message();
  EXPECT_EQ(*added, 1U);
  EXPECT_EQ((*set_r)->scene_count(), 2U);
  EXPECT_NE((*set_r)->scene(0), nullptr);
  EXPECT_NE((*set_r)->scene(1), nullptr);

  {
    auto reports = (*set_r)->test();
    ASSERT_TRUE(reports.has_value()) << reports.error().message();
    EXPECT_EQ(reports->size(), 2U);
  }

  // remove_scene leaves a hole at index 0; scene_count stays at 2.
  (*set_r)->remove_scene(0);
  EXPECT_EQ((*set_r)->scene_count(), 2U);
  EXPECT_EQ((*set_r)->scene(0), nullptr);
  EXPECT_NE((*set_r)->scene(1), nullptr);

  // Hole + one engaged scene: combined commit still lands; the hole
  // contributes a zero CommitReport at its index.
  {
    auto reports = (*set_r)->test();
    ASSERT_TRUE(reports.has_value()) << reports.error().message();
    ASSERT_EQ(reports->size(), 2U);
    EXPECT_EQ((*reports)[0].layers_total, 0U);
  }

  // add_layer must reject the hole index now even though it's in range.
  {
    auto shared_src = drm::scene::DumbBufferSource::create(*fx.dev, 64, 64, DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(shared_src.has_value()) << shared_src.error().message();
    const std::shared_ptr<drm::scene::LayerBufferSource> src(std::move(*shared_src));

    drm::scene::SceneSetLayerSpec spec;
    spec.source = src;
    spec.targets.push_back({.scene_index = 0, .display = {}, .force_composited = false});
    auto h = (*set_r)->add_layer(spec);
    ASSERT_FALSE(h.has_value());
    EXPECT_EQ(h.error(), std::make_error_code(std::errc::invalid_argument));
  }

  // A fresh add_scene reuses the lowest hole rather than appending.
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = fx.outputs[0].crtc_id;
  cfg.connector_id = fx.outputs[0].connector_id;
  cfg.mode = fx.outputs[0].mode;
  auto rebuilt = drm::scene::LayerScene::create(*fx.dev, cfg);
  ASSERT_TRUE(rebuilt.has_value()) << rebuilt.error().message();
  auto refilled = (*set_r)->add_scene(std::move(*rebuilt));
  ASSERT_TRUE(refilled.has_value());
  EXPECT_EQ(*refilled, 0U);
  EXPECT_EQ((*set_r)->scene_count(), 2U);
  EXPECT_NE((*set_r)->scene(0), nullptr);
}

// AutoOnModeset split round-trip. Commit one scene first so it's
// steady-state, then add_scene a fresh second scene (first_commit_
// pending → would_request_modeset == true). NarrowPolicy::AutoOnModeset
// should split the next commit into two ioctls (modeset-needing first,
// steady second) and both scenes' reports should reflect a successful
// frame. After this split commit, both scenes are steady, so the
// follow-up AutoOnModeset commit collapses back to one combined ioctl.
TEST(SceneSetVkms, AutoOnModesetSplitsThenReconverges) {
  const auto node = find_vkms_multi_crtc(2);
  if (!node) {
    GTEST_SKIP() << "no vkms instance with >=2 connected outputs";
  }
  auto fx_r = open_scenes(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  ASSERT_GE(fx.scenes.size(), 2U);

  // Build SceneSet with just scene[0]. scene[1] stays on the side.
  std::unique_ptr<drm::scene::LayerScene> spare = std::move(fx.scenes[1]);
  fx.scenes.pop_back();
  auto set_r = drm::scene::SceneSet::create(*fx.dev, std::move(fx.scenes));
  ASSERT_TRUE(set_r.has_value()) << set_r.error().message();

  // Layer for scene[0].
  {
    const auto& o0 = fx.outputs[0];
    auto src = drm::scene::DumbBufferSource::create(*fx.dev, o0.mode.hdisplay, o0.mode.vdisplay,
                                                    DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = drm::scene::Rect{0, 0, o0.mode.hdisplay, o0.mode.vdisplay};
    desc.display.dst_rect = drm::scene::Rect{0, 0, o0.mode.hdisplay, o0.mode.vdisplay};
    ASSERT_TRUE((*set_r)->scene(0));
    auto h = (*set_r)->scene(0)->add_layer(std::move(desc));
    ASSERT_TRUE(h.has_value()) << h.error().message();
  }

  // Real commit to flip scene[0] past first_commit_.
  {
    auto reports = (*set_r)->commit(0, nullptr, drm::scene::NarrowPolicy::AutoOnModeset);
    ASSERT_TRUE(reports.has_value()) << reports.error().message();
    ASSERT_EQ(reports->size(), 1U);
  }

  // Add the second scene + a layer. would_request_modeset is true on
  // the fresh scene; the existing scene[0] is steady. Mixed state →
  // AutoOnModeset should split.
  const auto& o1 = fx.outputs[1];
  {
    auto src = drm::scene::DumbBufferSource::create(*fx.dev, o1.mode.hdisplay, o1.mode.vdisplay,
                                                    DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = drm::scene::Rect{0, 0, o1.mode.hdisplay, o1.mode.vdisplay};
    desc.display.dst_rect = drm::scene::Rect{0, 0, o1.mode.hdisplay, o1.mode.vdisplay};
    auto h = spare->add_layer(std::move(desc));
    ASSERT_TRUE(h.has_value()) << h.error().message();
  }

  EXPECT_FALSE((*set_r)->scene(0)->would_request_modeset())
      << "scene[0] should be steady after first commit";
  EXPECT_TRUE(spare->would_request_modeset())
      << "fresh scene should report modeset-pending pre-add";

  auto added = (*set_r)->add_scene(std::move(spare));
  ASSERT_TRUE(added.has_value()) << added.error().message();
  EXPECT_EQ((*set_r)->scene_count(), 2U);

  // Real commit under AutoOnModeset; the kernel sees two separate
  // commits in sequence. Both scenes' reports should reflect their
  // active layer.
  {
    auto reports = (*set_r)->commit(0, nullptr, drm::scene::NarrowPolicy::AutoOnModeset);
    ASSERT_TRUE(reports.has_value()) << reports.error().message();
    ASSERT_EQ(reports->size(), 2U);
    EXPECT_EQ((*reports)[0].layers_total, 1U);
    EXPECT_EQ((*reports)[1].layers_total, 1U);
  }

  // After the split commit, both scenes have flipped past first_commit_.
  // The next AutoOnModeset commit should reconverge to one combined
  // group, and a TEST call should also accept.
  EXPECT_FALSE((*set_r)->scene(0)->would_request_modeset());
  EXPECT_FALSE((*set_r)->scene(1)->would_request_modeset());

  auto follow = (*set_r)->test(drm::scene::NarrowPolicy::AutoOnModeset);
  ASSERT_TRUE(follow.has_value()) << follow.error().message();
  EXPECT_EQ(follow->size(), 2U);
}

// User-set HDR also flips would_request_modeset, so SceneSet's
// AutoOnModeset partitions correctly when an application calls
// set_output_metadata on one scene of a steady-state set. vkms doesn't
// expose HDR_OUTPUT_METADATA so the actual modeset is a no-op
// kernel-side, but the scene-local hdr_dirty_pending_ flag still
// flips and SceneSet's pre-build peek picks it up.
TEST(SceneSetVkms, AutoOnModesetSplitsOnUserSetHdr) {
  const auto node = find_vkms_multi_crtc(2);
  if (!node) {
    GTEST_SKIP() << "no vkms instance with >=2 connected outputs";
  }
  auto fx_r = open_scenes(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  ASSERT_GE(fx.scenes.size(), 2U);

  // Layers on every scene so a commit has writes to land.
  for (std::size_t i = 0; i < fx.scenes.size(); ++i) {
    const auto& o = fx.outputs[i];
    auto src = drm::scene::DumbBufferSource::create(*fx.dev, o.mode.hdisplay, o.mode.vdisplay,
                                                    DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    desc.display.dst_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    auto h = fx.scenes[i]->add_layer(std::move(desc));
    ASSERT_TRUE(h.has_value()) << h.error().message();
  }

  drm::scene::LayerScene* scene0 = fx.scenes[0].get();
  drm::scene::LayerScene* scene1 = fx.scenes[1].get();

  auto set_r = drm::scene::SceneSet::create(*fx.dev, std::move(fx.scenes));
  ASSERT_TRUE(set_r.has_value()) << set_r.error().message();

  // First commit takes both scenes through ALLOW_MODESET as one
  // combined group (uniform-modeset). After it lands, both scenes are
  // steady.
  {
    auto reports = (*set_r)->commit(0, nullptr, drm::scene::NarrowPolicy::AutoOnModeset);
    ASSERT_TRUE(reports.has_value()) << reports.error().message();
    ASSERT_EQ(reports->size(), 2U);
  }
  EXPECT_FALSE(scene0->would_request_modeset());
  EXPECT_FALSE(scene1->would_request_modeset());

  // User-set HDR on scene 0 flips the dirty flag, so AutoOnModeset
  // splits the next commit into [scene 0, scene 1]. The metadata
  // content is irrelevant to this test — only the would_request_modeset
  // hint is being exercised; vkms has no HDR_OUTPUT_METADATA property
  // anyway, so the kernel-side write is a documented no-op.
  drm::display::HdrSourceMetadata md{};
  md.eotf = drm::display::TransferFunction::SmpteSt2084Pq;
  md.max_display_mastering_luminance = 1000U;
  md.min_display_mastering_luminance = 5U;
  md.max_content_light_level = 1000U;
  md.max_frame_average_light_level = 400U;
  scene0->set_output_metadata(md);

  EXPECT_TRUE(scene0->would_request_modeset())
      << "set_output_metadata should flip scene 0's modeset hint";
  EXPECT_FALSE(scene1->would_request_modeset()) << "scene 1 wasn't touched, hint should stay false";

  // SceneSet sees mixed modeset state and splits into modeset-needing
  // (scene 0) first, then steady (scene 1). Both commits land on vkms
  // (the connector property is absent, so the modeset write itself is
  // a no-op kernel-side).
  {
    auto reports = (*set_r)->commit(0, nullptr, drm::scene::NarrowPolicy::AutoOnModeset);
    ASSERT_TRUE(reports.has_value()) << reports.error().message();
    ASSERT_EQ(reports->size(), 2U);
  }

  // Post-commit the dirty flag is cleared on scene 0 and both scenes
  // are back to steady. The follow-up AutoOnModeset commit collapses
  // back to one combined group.
  EXPECT_FALSE(scene0->would_request_modeset());
  EXPECT_FALSE(scene1->would_request_modeset());
}

// PerCrtc forces a per-scene ioctl unconditionally; verify it also
// accepts on vkms with two engaged scenes and produces one report per
// scene with the layer counts the underlying scene assignments
// reported.
TEST(SceneSetVkms, PerCrtcAcceptsTwoEngagedScenes) {
  const auto node = find_vkms_multi_crtc(2);
  if (!node) {
    GTEST_SKIP() << "no vkms instance with >=2 connected outputs";
  }
  auto fx_r = open_scenes(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  for (std::size_t i = 0; i < fx.outputs.size(); ++i) {
    const auto& o = fx.outputs[i];
    auto src = drm::scene::DumbBufferSource::create(*fx.dev, o.mode.hdisplay, o.mode.vdisplay,
                                                    DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    desc.display.dst_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    auto h = fx.scenes[i]->add_layer(std::move(desc));
    ASSERT_TRUE(h.has_value()) << h.error().message();
  }

  auto set_r = drm::scene::SceneSet::create(*fx.dev, std::move(fx.scenes));
  ASSERT_TRUE(set_r.has_value()) << set_r.error().message();

  auto reports = (*set_r)->test(drm::scene::NarrowPolicy::PerCrtc);
  ASSERT_TRUE(reports.has_value()) << reports.error().message();
  ASSERT_EQ(reports->size(), fx.outputs.size());
  for (std::size_t i = 0; i < reports->size(); ++i) {
    EXPECT_EQ((*reports)[i].layers_total, 1U) << "scene " << i;
  }
}
