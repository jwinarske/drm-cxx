// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Phase 2.4 (rebind half) integration coverage. VKMS exposes a single
// hardcoded mode per connector by default, so we can't drive a real
// mode change here — the tests instead pin the documented contracts
// of `LayerScene::rebind` against a no-op rebind to the same
// crtc/connector/mode:
//
//   1. Layer handles survive verbatim. Both id and generation are
//      preserved, and `get_layer(handle)` after the rebind returns a
//      live Layer.
//   2. The next commit succeeds (the scene re-took ALLOW_MODESET, the
//      MODE_ID blob was rebuilt, and the per-plane property snapshot
//      was reset so we don't try to skip writes against a stale
//      baseline).
//   3. CompatibilityReport flags layers whose dst_rect lies entirely
//      outside the new mode's display area.
//
// Self-skips when VKMS isn't loaded.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/compatibility_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;
using drm::Device;
using drm::scene::CompatibilityReport;
using drm::scene::DumbBufferSource;
using drm::scene::LayerDesc;
using drm::scene::LayerIncompatibility;
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

TEST(LayerSceneRebindVkms, NoOpRebindPreservesHandlesAndLetsCommitSucceed) {
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
  auto handle_r = fx.scene->add_layer(std::move(bg));
  ASSERT_TRUE(handle_r.has_value());
  const auto handle = *handle_r;

  ASSERT_TRUE(fx.scene->commit().has_value());

  // No-op rebind: same crtc, connector, mode. Nothing's actually
  // changing on the kernel side, but the scene's internal state goes
  // through the full teardown + rebuild path. The interesting
  // observable: handle survives, next commit succeeds.
  auto report = fx.scene->rebind(fx.active.crtc_id, fx.active.connector_id, fx.active.mode);
  ASSERT_TRUE(report.has_value()) << report.error().message();
  EXPECT_TRUE(report->empty()) << "no layer should be flagged for a no-op rebind";

  // Handle still resolves to a live layer post-rebind.
  EXPECT_NE(fx.scene->get_layer(handle), nullptr) << "layer handle did not survive rebind";

  // Next commit must succeed — the scene should have re-armed
  // ALLOW_MODESET internally and rebuilt the MODE_ID blob.
  auto post_commit = fx.scene->commit();
  ASSERT_TRUE(post_commit.has_value()) << post_commit.error().message();
  EXPECT_GT(post_commit->properties_written, 0U)
      << "first commit after rebind should be a full property emit "
         "(per-plane snapshot was reset)";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

// Proposal-2 round-trip. The scene resolves a layer by its opaque
// identity_tag without a parallel embedder-side map, and the mapping
// survives rebind() — the embedder's identity contract holds across
// the same scene transitions LayerHandle is documented stable across.
TEST(LayerSceneRebindVkms, FindByIdentityTagRoundTripAndSurvivesRebind) {
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

  // Two layers, two distinct identity tags. The pointer values stand
  // in for engine-side identities (e.g. FlutterBackingStore*); the
  // scene never dereferences them.
  int tag_a = 0;
  int tag_b = 0;

  auto a_source = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(a_source.has_value());
  LayerDesc a_desc;
  a_desc.source = std::move(*a_source);
  a_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  a_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  a_desc.display.zpos = 1;
  a_desc.identity_tag = &tag_a;
  auto a_handle_r = fx.scene->add_layer(std::move(a_desc));
  ASSERT_TRUE(a_handle_r.has_value());
  const auto a_handle = *a_handle_r;

  auto b_source = DumbBufferSource::create(*fx.dev, fb_w / 2U, fb_h / 2U, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(b_source.has_value());
  LayerDesc b_desc;
  b_desc.source = std::move(*b_source);
  b_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w / 2U, fb_h / 2U};
  b_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w / 2U, fb_h / 2U};
  b_desc.display.zpos = 2;
  b_desc.identity_tag = &tag_b;
  auto b_handle_r = fx.scene->add_layer(std::move(b_desc));
  ASSERT_TRUE(b_handle_r.has_value());
  const auto b_handle = *b_handle_r;

  // Round-trip: each tag resolves back to the corresponding live Layer,
  // and the resolved Layer's handle matches the original add_layer
  // handle. Nullptr is the unset sentinel and matches nothing.
  auto* a_layer = fx.scene->find_by_identity_tag(&tag_a);
  ASSERT_NE(a_layer, nullptr);
  EXPECT_EQ(a_layer->handle().id, a_handle.id);
  auto* b_layer = fx.scene->find_by_identity_tag(&tag_b);
  ASSERT_NE(b_layer, nullptr);
  EXPECT_EQ(b_layer->handle().id, b_handle.id);
  EXPECT_EQ(fx.scene->find_by_identity_tag(nullptr), nullptr);

  // Unknown tag: not found, no crash.
  int unknown_tag = 0;
  EXPECT_EQ(fx.scene->find_by_identity_tag(&unknown_tag), nullptr);

  ASSERT_TRUE(fx.scene->commit().has_value());

  // Survive a no-op rebind — same crtc / connector / mode. The scene
  // tears down + rebuilds internal state; identity_tag rides along on
  // the Layer because the Layer object itself is preserved across the
  // transition (LayerHandle is documented stable across rebind, and
  // identity_tag is a non-mutating field on the same object).
  auto report = fx.scene->rebind(fx.active.crtc_id, fx.active.connector_id, fx.active.mode);
  ASSERT_TRUE(report.has_value()) << report.error().message();

  auto* a_after = fx.scene->find_by_identity_tag(&tag_a);
  ASSERT_NE(a_after, nullptr);
  EXPECT_EQ(a_after->handle().id, a_handle.id);
  auto* b_after = fx.scene->find_by_identity_tag(&tag_b);
  ASSERT_NE(b_after, nullptr);
  EXPECT_EQ(b_after->handle().id, b_handle.id);

  // Remove a layer; its tag stops resolving. The other tag is
  // unaffected.
  fx.scene->remove_layer(a_handle);
  EXPECT_EQ(fx.scene->find_by_identity_tag(&tag_a), nullptr);
  auto* b_still = fx.scene->find_by_identity_tag(&tag_b);
  ASSERT_NE(b_still, nullptr);
  EXPECT_EQ(b_still->handle().id, b_handle.id);

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

TEST(LayerSceneRebindVkms, OffScreenLayerFlaggedInCompatibilityReport) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  // On-screen layer: full screen, will not be flagged.
  auto on_source = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(on_source.has_value());
  LayerDesc on_desc;
  on_desc.source = std::move(*on_source);
  on_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  on_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  on_desc.display.zpos = 1;
  auto on_handle = fx.scene->add_layer(std::move(on_desc));
  ASSERT_TRUE(on_handle.has_value());

  // Off-screen layer: dst_rect entirely beyond the right edge.
  auto off_source = DumbBufferSource::create(*fx.dev, fb_w / 4U, fb_h / 4U, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(off_source.has_value());
  LayerDesc off_desc;
  off_desc.source = std::move(*off_source);
  off_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w / 4U, fb_h / 4U};
  off_desc.display.dst_rect =
      drm::scene::Rect{static_cast<std::int32_t>(fb_w + 100), 0, fb_w / 4U, fb_h / 4U};
  off_desc.display.zpos = 2;
  auto off_handle = fx.scene->add_layer(std::move(off_desc));
  ASSERT_TRUE(off_handle.has_value());

  // Rebind to the same configuration. The off-screen layer should be
  // flagged; the on-screen layer should not.
  auto report = fx.scene->rebind(fx.active.crtc_id, fx.active.connector_id, fx.active.mode);
  ASSERT_TRUE(report.has_value()) << report.error().message();
  ASSERT_EQ(report->incompatibilities.size(), 1U);
  EXPECT_EQ(report->incompatibilities[0].handle.id, off_handle->id);
  EXPECT_EQ(report->incompatibilities[0].handle.generation, off_handle->generation);
  EXPECT_EQ(report->incompatibilities[0].reason, LayerIncompatibility::Reason::DstRectOffScreen);

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}
