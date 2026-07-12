// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Exercises per-layer application placement priority against a live KMS
// CRTC. Several overlapping Video layers contend for too few planes; the
// allocator must keep the highest-priority layers on planes and drop the
// lowest-priority ones to composition. The app_priority values are
// shuffled relative to insertion order, so a pass proves the drop follows
// app_priority and not the layer's position in the scene.
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

#include <algorithm>
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
using drm::planes::ContentType;
using drm::scene::DumbBufferSource;
using drm::scene::LayerDesc;
using drm::scene::LayerHandle;
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

// DRM_CXX_TEST_CARD override first (so a hardware run on e.g. vc4 is never
// shadowed by a loaded vkms), else the vkms node.
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

// Overlapping Video layers with shuffled app_priority contend for scarce
// planes. Whatever the plane count, no lower-priority layer may hold a
// plane while a higher-priority layer is dropped to composition.
TEST(LayerSceneAppPriorityVkms, LowestPriorityDropsUnderPlanePressure) {
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

  // A full-screen background so composition always has a canvas to land on.
  auto bg_source = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg_source.has_value()) << bg_source.error().message();
  LayerDesc bg;
  bg.source = std::move(*bg_source);
  bg.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg.display.zpos = 0;
  ASSERT_TRUE(fx.scene->add_layer(std::move(bg)).has_value());

  // Five fully-overlapping Video layers (one spatial contention group).
  // app_priority is shuffled versus insertion order, so a pass proves the
  // drop tracks app_priority rather than the layer's index.
  const std::vector<std::uint8_t> priorities = {50, 10, 40, 20, 30};
  const std::uint32_t cw = fb_w / 2;
  const std::uint32_t ch = fb_h / 2;
  std::vector<std::pair<LayerHandle, std::uint8_t>> layers;
  for (std::size_t i = 0; i < priorities.size(); ++i) {
    auto src = DumbBufferSource::create(*fx.dev, cw, ch, DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    LayerDesc d;
    d.source = std::move(*src);
    d.display.src_rect = drm::scene::Rect{0, 0, cw, ch};
    d.display.dst_rect = drm::scene::Rect{0, 0, cw, ch};
    d.display.zpos = static_cast<int>(i) + 1;
    d.content_type = ContentType::Video;
    d.app_priority = priorities.at(i);
    auto h = fx.scene->add_layer(std::move(d));
    ASSERT_TRUE(h.has_value()) << h.error().message();
    layers.emplace_back(*h, priorities.at(i));
  }

  ASSERT_TRUE(fx.scene->commit().has_value());

  // Partition the Video layers into plane-backed vs dropped-to-composition.
  int worst_placed = 256;  // min app_priority among plane-backed layers
  int best_dropped = -1;   // max app_priority among dropped layers
  int placed = 0;
  int dropped = 0;
  for (const auto& [handle, prio] : layers) {
    auto* layer = fx.scene->get_layer(handle);
    ASSERT_NE(layer, nullptr);
    if (layer->last_placement() == LayerPlacement::AssignedToPlane) {
      ++placed;
      worst_placed = std::min(worst_placed, static_cast<int>(prio));
    } else {
      ++dropped;
      best_dropped = std::max(best_dropped, static_cast<int>(prio));
    }
  }

  if (dropped == 0) {
    cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
    GTEST_SKIP() << "device placed all " << placed
                 << " layers — no plane pressure, cannot exercise priority drop";
  }

  // The core invariant: the lowest-priority plane-backed layer must still
  // out-rank the highest-priority dropped layer. Equivalently, the dropped
  // set is exactly the lowest-app_priority layers.
  EXPECT_GT(worst_placed, best_dropped)
      << "a lower-priority layer held a plane while a higher-priority layer was dropped "
      << "(placed=" << placed << " dropped=" << dropped << ")";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}
