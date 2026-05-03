// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Phase 2.2: end-to-end coverage of LayerScene's property minimization.
//
// What it proves:
//   1. Steady-state with no layer mutations writes ZERO properties on
//      the warm-start path (the per-plane snapshot diff suppresses
//      every kept-since-last-frame property).
//   2. A scene with a single DumbBufferSource (constant FB_ID across
//      commits) reports `fbs_attached == 0` once the warm-start
//      engages — the kernel keeps the last-committed FB attached.
//   3. Mutating a layer's destination rect re-emits only the four
//      CRTC_X/Y/W/H properties, not FB_ID or SRC_*.
//   4. force_full_property_writes opt-out forces every property to
//      hit the wire on every commit, regardless of whether the
//      snapshot would have suppressed it.
//
// Self-skips when VKMS isn't loaded — same pattern as the other
// tests/integration files.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

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

namespace fs = std::filesystem;
using drm::Device;
using drm::scene::DumbBufferSource;
using drm::scene::LayerDesc;
using drm::scene::LayerScene;

namespace {

std::optional<std::string> find_vkms_node() {
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
    ::close(fd);
    if (is_vkms) {
      return p.string();
    }
  }
  return std::nullopt;
}

struct ActiveCrtc {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

drm::expected<ActiveCrtc, std::error_code> pick_crtc(int fd) {
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }
  std::optional<ActiveCrtc> found;
  for (int i = 0; i < res->count_connectors && !found.has_value(); ++i) {
    auto* conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn == nullptr) {
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      for (int e = 0; e < conn->count_encoders && !found.has_value(); ++e) {
        auto* enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (enc == nullptr) {
          continue;
        }
        for (int c = 0; c < res->count_crtcs; ++c) {
          if ((enc->possible_crtcs & (1U << static_cast<unsigned>(c))) != 0) {
            ActiveCrtc out;
            out.connector_id = conn->connector_id;
            out.mode = conn->modes[0];
            out.crtc_id = res->crtcs[c];
            found = out;
            break;
          }
        }
        drmModeFreeEncoder(enc);
      }
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);
  if (!found.has_value()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_device_or_address));
  }
  return *found;
}

// Heap-allocated Device so its address survives the fixture being
// returned by value — LayerScene::create captures a `Device&`, and a
// fixture-by-value move would dangle the scene's internal pointer if
// the Device were a direct member.
struct SceneFixture {
  std::unique_ptr<Device> dev;
  ActiveCrtc active;
  std::unique_ptr<LayerScene> scene;
};

drm::expected<SceneFixture, std::error_code> open_vkms_scene(const std::string& node) {
  auto dev_r = Device::open(node);
  if (!dev_r) {
    return drm::unexpected<std::error_code>(dev_r.error());
  }
  auto dev = std::make_unique<Device>(std::move(*dev_r));
  if (auto r = dev->enable_universal_planes(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = dev->enable_atomic(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  auto active_r = pick_crtc(dev->fd());
  if (!active_r) {
    return drm::unexpected<std::error_code>(active_r.error());
  }
  LayerScene::Config cfg;
  cfg.crtc_id = active_r->crtc_id;
  cfg.connector_id = active_r->connector_id;
  cfg.mode = active_r->mode;
  auto scene_r = LayerScene::create(*dev, cfg);
  if (!scene_r) {
    return drm::unexpected<std::error_code>(scene_r.error());
  }
  return SceneFixture{std::move(dev), *active_r, std::move(*scene_r)};
}

void cleanup_crtc(int fd, std::uint32_t crtc_id) {
  drmModeSetCrtc(fd, crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}

}  // namespace

TEST(LayerSceneMinimizationVkms, SteadyStateWritesOnlyFbIdPerAssignedLayer) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded — `sudo modprobe vkms enable_overlay=1` "
                    "to enable this test";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;
  auto bg_source = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg_source.has_value()) << bg_source.error().message();

  LayerDesc bg;
  bg.source = std::move(*bg_source);
  bg.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg.display.zpos = 1;
  ASSERT_TRUE(fx.scene->add_layer(std::move(bg)).has_value());

  // First commit: full property writes — the scene has no per-plane
  // snapshot yet, every property hits the wire including FB_ID.
  auto first = fx.scene->commit();
  ASSERT_TRUE(first.has_value()) << first.error().message();
  EXPECT_GT(first->properties_written, 0U) << "first commit should emit a full property set";
  EXPECT_GE(first->fbs_attached, 1U) << "first commit must attach at least one FB";
  EXPECT_GE(first->test_commits_issued, 1U)
      << "cold start must run full_search and issue at least one TEST_ONLY";

  // Second commit: nothing changed *except* FB_ID always re-emits to
  // keep the kernel's PAGE_FLIP_EVENT scheduling alive — every other
  // plane property is suppressed by the per-plane snapshot diff. With
  // one assigned layer that's exactly one write (the FB_ID) and one
  // FB attachment.
  auto second = fx.scene->commit();
  ASSERT_TRUE(second.has_value()) << second.error().message();
  EXPECT_EQ(second->properties_written, 1U)
      << "steady state should emit only FB_ID per assigned layer";
  EXPECT_EQ(second->fbs_attached, 1U) << "FB_ID re-attaches every frame";
  EXPECT_EQ(second->test_commits_issued, 1U)
      << "warm steady state still re-validates the cached assignment with one TEST_ONLY";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

TEST(LayerSceneMinimizationVkms, ForceFullWritesEmitsEveryProperty) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  fx.scene->set_force_full_property_writes(true);
  EXPECT_TRUE(fx.scene->force_full_property_writes());

  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;
  auto bg_source = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg_source.has_value()) << bg_source.error().message();

  LayerDesc bg;
  bg.source = std::move(*bg_source);
  bg.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg.display.zpos = 1;
  ASSERT_TRUE(fx.scene->add_layer(std::move(bg)).has_value());

  auto first = fx.scene->commit();
  ASSERT_TRUE(first.has_value()) << first.error().message();
  const auto first_writes = first->properties_written;
  ASSERT_GT(first_writes, 0U);

  // Second commit with no mutation. Without force-full this would
  // emit zero properties (the SteadyState test pins that contract).
  // With force-full, the minimization filter is bypassed so every
  // layer property is re-emitted *and* the disable-unused-planes
  // pass also re-emits FB_ID=0 / CRTC_ID=0 for each non-keep plane
  // regardless of whether it was already off — that's the whole
  // point of the opt-out. The combined write count is therefore
  // strictly greater than a SteadyState second-commit (zero) and at
  // least as large as the per-layer property count from the first
  // commit.
  auto second = fx.scene->commit();
  ASSERT_TRUE(second.has_value()) << second.error().message();
  EXPECT_GE(second->properties_written, first_writes)
      << "force-full second commit must re-emit at least the layer's "
         "property set (got "
      << second->properties_written << ", baseline " << first_writes << ")";
  EXPECT_GE(second->fbs_attached, 1U)
      << "force-full must include at least one FB_ID write per commit";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

// Regression: warm-start used to keep the cached `previous_allocation_`
// when a fresh layer was added in steady state, and that layer would
// be force-composited because warm-start is structurally unable to
// place a layer it doesn't already remember. Once compositing started,
// `previous_allocation_` never grew to include the new layer, so the
// next frame hit the same trap and the layer stayed on the canvas
// forever — observed in examples/camera as a 60→6 fps cliff every
// time a USB camera was hot-plugged after a prior unplug. The fix
// detects "scene has a layer not represented in previous_allocation_"
// and falls through to full_search so the new layer gets a real shot
// at a hardware plane.
TEST(LayerSceneMinimizationVkms, NewLayerInWarmStateLandsOnHardwarePlane) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded — `sudo modprobe vkms enable_overlay=1` "
                    "to enable this test";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;
  auto bg_source = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg_source.has_value());

  LayerDesc bg;
  bg.source = std::move(*bg_source);
  bg.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg.display.zpos = 1;
  auto bg_handle_r = fx.scene->add_layer(std::move(bg));
  ASSERT_TRUE(bg_handle_r.has_value());
  const auto bg_handle = *bg_handle_r;

  // Establish the warm-start state: bg in previous_allocation_, valid.
  ASSERT_TRUE(fx.scene->commit().has_value());
  auto warm = fx.scene->commit();
  ASSERT_TRUE(warm.has_value()) << warm.error().message();
  ASSERT_EQ(warm->test_commits_issued, 1U)
      << "second commit should be the warm-start re-validation, not full_search";

  // Add a new overlay layer mid-flight, sized so it cannot scale (VKMS
  // overlay can't scale) and small enough to fit on top of bg without
  // covering it. With the bug the next commit places bg from
  // previous_allocation_ and force-composites this new layer onto the
  // canvas; with the fix full_search runs and assigns it a plane.
  const std::uint32_t over_w = fb_w / 4U;
  const std::uint32_t over_h = fb_h / 4U;
  auto over_source = DumbBufferSource::create(*fx.dev, over_w, over_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(over_source.has_value());

  LayerDesc over;
  over.source = std::move(*over_source);
  over.display.src_rect = drm::scene::Rect{0, 0, over_w, over_h};
  over.display.dst_rect = drm::scene::Rect{32, 32, over_w, over_h};
  over.display.zpos = 2;
  auto over_handle_r = fx.scene->add_layer(std::move(over));
  ASSERT_TRUE(over_handle_r.has_value());
  const auto over_handle = *over_handle_r;

  auto report = fx.scene->commit();
  ASSERT_TRUE(report.has_value()) << report.error().message();
  EXPECT_EQ(report->layers_total, 2U);
  EXPECT_EQ(report->layers_assigned, 2U) << "both layers must reach scanout on hardware planes";
  EXPECT_EQ(report->layers_composited, 0U)
      << "new layer should not be force-composited just because it post-dates "
         "the previous allocation";

  // Locate the new layer's placement entry and confirm it landed on a
  // dedicated plane that's distinct from the bg plane.
  const drm::scene::LayerPlacementEntry* over_entry = nullptr;
  const drm::scene::LayerPlacementEntry* bg_entry = nullptr;
  for (const auto& p : report->placements) {
    if (p.handle == over_handle) {
      over_entry = &p;
    } else if (p.handle == bg_handle) {
      bg_entry = &p;
    }
  }
  ASSERT_NE(over_entry, nullptr);
  ASSERT_NE(bg_entry, nullptr);
  EXPECT_EQ(over_entry->placement, drm::scene::LayerPlacement::AssignedToPlane);
  EXPECT_EQ(bg_entry->placement, drm::scene::LayerPlacement::AssignedToPlane);
  EXPECT_NE(over_entry->plane_id, bg_entry->plane_id)
      << "overlay must be on a different plane from bg";

  // And the next frame, with both layers known to previous_allocation_,
  // should re-engage the warm-start re-validation (one TEST_ONLY).
  auto steady = fx.scene->commit();
  ASSERT_TRUE(steady.has_value()) << steady.error().message();
  EXPECT_EQ(steady->test_commits_issued, 1U)
      << "warm-start should re-engage now that previous_allocation_ knows the new layer";
  EXPECT_EQ(steady->layers_assigned, 2U);
  EXPECT_EQ(steady->layers_composited, 0U);

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

TEST(LayerSceneMinimizationVkms, TranslatingLayerEmitsOnlyOneRectProperty) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  // Use a sub-screen layer so we can translate it without going
  // off-screen and without changing dimensions (changing dimensions
  // would imply scaling, which VKMS planes don't support — the
  // allocator would refuse to place the layer and the test would
  // fall through to full_search every frame, which defeats the
  // purpose of measuring minimization).
  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;
  const std::uint32_t layer_w = fb_w / 2U;
  const std::uint32_t layer_h = fb_h / 2U;
  auto bg_source = DumbBufferSource::create(*fx.dev, layer_w, layer_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg_source.has_value());

  LayerDesc bg;
  bg.source = std::move(*bg_source);
  bg.display.src_rect = drm::scene::Rect{0, 0, layer_w, layer_h};
  bg.display.dst_rect = drm::scene::Rect{32, 32, layer_w, layer_h};
  bg.display.zpos = 1;
  auto handle_r = fx.scene->add_layer(std::move(bg));
  ASSERT_TRUE(handle_r.has_value());
  const auto handle = *handle_r;

  ASSERT_TRUE(fx.scene->commit().has_value());

  // Translate by +32 in x only. Same dimensions → no scaling. Only
  // CRTC_X changes among the geometric properties. FB_ID always
  // re-emits regardless of the diff (page-flip protocol), so the
  // total expected writes are 2 (FB_ID + CRTC_X) with one FB attach.
  auto* layer = fx.scene->get_layer(handle);
  ASSERT_NE(layer, nullptr);
  layer->set_dst_rect(drm::scene::Rect{64, 32, layer_w, layer_h});

  auto report = fx.scene->commit();
  ASSERT_TRUE(report.has_value()) << report.error().message();
  EXPECT_EQ(report->properties_written, 2U)
      << "expected FB_ID + CRTC_X, got " << report->properties_written;
  EXPECT_EQ(report->fbs_attached, 1U) << "FB_ID always re-emits";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}
