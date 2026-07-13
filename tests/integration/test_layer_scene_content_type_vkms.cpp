// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Exercises mutable LayerScene hints (content_type / update_hint_hz)
// against a live KMS CRTC:
//   * set_content_type / set_update_hint flip the layer's hints_dirty
//     flag, which a successful commit clears (mark_clean);
//   * the hint change forces the allocator to drop its warm-start and
//     re-run a full search that frame (the layer stays placed, and the
//     next frame returns to the warm-start steady state).
//
// Runs against VKMS by default (`sudo modprobe vkms enable_overlay=1
// enable_plane_pipeline=1`) and self-skips when it isn't loaded, so it's
// green on runners that haven't modprobed it. Set DRM_CXX_TEST_CARD=
// /dev/dri/cardN to run it against real hardware instead (e.g. vc4 on a
// Raspberry Pi) — the logic is driver-agnostic. Requires DRM master.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/planes/layer.hpp>
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

namespace fs = std::filesystem;

using drm::Device;
using drm::planes::ContentType;
using drm::scene::DumbBufferSource;
using drm::scene::LayerDesc;
using drm::scene::LayerPlacement;
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

// The KMS node to run against: a DRM_CXX_TEST_CARD override first (so a
// hardware run on e.g. vc4 is never shadowed by a loaded vkms), else the
// vkms node. Matches the convention in the ring scene test.
std::optional<std::string> find_scene_card() {
  if (const char* node = std::getenv("DRM_CXX_TEST_CARD"); node != nullptr && *node != '\0') {
    return std::string(node);
  }
  return find_vkms_node();
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

TEST(LayerSceneContentTypeVkms, ContentTypeChangeForcesReallocation) {
  const auto node = find_scene_card();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded — `sudo modprobe vkms enable_overlay=1 "
                    "enable_plane_pipeline=1` to enable this test";
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

  // A smaller layer above the bg, added as Generic — the candidate we
  // later promote to Video.
  const std::uint32_t cw = fb_w / 2;
  const std::uint32_t ch = fb_h / 2;
  auto cand_source = DumbBufferSource::create(*fx.dev, cw, ch, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(cand_source.has_value()) << cand_source.error().message();
  LayerDesc cand;
  cand.source = std::move(*cand_source);
  cand.display.src_rect = drm::scene::Rect{0, 0, cw, ch};
  cand.display.dst_rect = drm::scene::Rect{0, 0, cw, ch};
  cand.display.zpos = 2;
  cand.content_type = ContentType::Generic;
  auto handle_r = fx.scene->add_layer(std::move(cand));
  ASSERT_TRUE(handle_r.has_value()) << handle_r.error().message();
  const auto handle = *handle_r;

  // Two commits to reach the warm-start steady state (second commit
  // reuses the previous allocation: one TEST_ONLY).
  ASSERT_TRUE(fx.scene->commit().has_value());
  auto steady = fx.scene->commit();
  ASSERT_TRUE(steady.has_value()) << steady.error().message();
  EXPECT_EQ(steady->test_commits_issued, 0U)
      << "steady state takes the FB-only fast path — no redundant TEST_ONLY";
  EXPECT_TRUE(steady->fb_delta_fast_path);

  // Promote the candidate to Video. The setter flags the layer.
  auto* layer = fx.scene->get_layer(handle);
  ASSERT_NE(layer, nullptr);
  EXPECT_FALSE(layer->hints_dirty());
  layer->set_content_type(ContentType::Video);
  EXPECT_TRUE(layer->hints_dirty());
  EXPECT_EQ(layer->content_type(), ContentType::Video);

  // The commit drops warm-start, re-runs the allocation, and clears the
  // flag via mark_clean. The layer stays on a real plane (or the canvas)
  // — it isn't dropped by the re-search.
  auto promoted = fx.scene->commit();
  ASSERT_TRUE(promoted.has_value()) << promoted.error().message();
  EXPECT_FALSE(fx.scene->get_layer(handle)->hints_dirty()) << "commit must clear hints_dirty";
  EXPECT_NE(layer->last_placement(), LayerPlacement::Unassigned)
      << "the promoted layer must still reach scanout after re-allocation";

  // The invalidation is a one-frame event: the next commit is a plain
  // warm-start reuse again.
  auto after = fx.scene->commit();
  ASSERT_TRUE(after.has_value()) << after.error().message();
  EXPECT_EQ(after->test_commits_issued, 0U)
      << "invalidation must not be sticky — the next steady frame is back on the fast path";
  EXPECT_TRUE(after->fb_delta_fast_path);

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

TEST(LayerSceneContentTypeVkms, UpdateHintChangeRoundTripsThroughCommit) {
  const auto node = find_scene_card();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  auto src = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(src.has_value()) << src.error().message();
  LayerDesc d;
  d.source = std::move(*src);
  d.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  d.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  d.display.zpos = 1;
  auto handle_r = fx.scene->add_layer(std::move(d));
  ASSERT_TRUE(handle_r.has_value());
  const auto handle = *handle_r;
  ASSERT_TRUE(fx.scene->commit().has_value());

  auto* layer = fx.scene->get_layer(handle);
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(layer->update_hint_hz(), 0U);
  layer->set_update_hint(30U);
  EXPECT_TRUE(layer->hints_dirty());
  EXPECT_EQ(layer->update_hint_hz(), 30U);

  ASSERT_TRUE(fx.scene->commit().has_value());
  EXPECT_FALSE(fx.scene->get_layer(handle)->hints_dirty());

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}
