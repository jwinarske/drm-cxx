// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Exercises LayerDesc::pin_to_plane against a live KMS CRTC. A layer
// pinned to a specific overlay plane must scan out on exactly that plane
// (never displaced by the allocator), and a pin that can't be honored
// must not silently drop the layer: it falls back to normal allocation
// and bumps CommitReport::pins_failed.
//
// Runs against VKMS by default (`sudo modprobe vkms enable_overlay=1
// enable_plane_pipeline=1`) and self-skips when it isn't loaded. Set
// DRM_CXX_TEST_CARD=/dev/dri/cardN to run against real hardware instead.
// Requires DRM master.

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
#include <drm_mode.h>
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

std::optional<std::string> find_scene_card() {
  if (const char* node = std::getenv("DRM_CXX_TEST_CARD"); node != nullptr && *node != '\0') {
    return std::string(node);
  }
  return find_vkms_node();
}

struct ActiveCrtc {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  int pipe{-1};  // index of crtc_id in resources->crtcs, for possible_crtcs
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
            out.pipe = c;
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

std::optional<std::uint64_t> plane_type(int fd, std::uint32_t plane_id) {
  auto* props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (props == nullptr) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> type;
  for (std::uint32_t i = 0; i < props->count_props; ++i) {
    auto* prop = drmModeGetProperty(fd, props->props[i]);
    if (prop != nullptr) {
      if (std::strcmp(prop->name, "type") == 0) {
        type = props->prop_values[i];
      }
      drmModeFreeProperty(prop);
    }
  }
  drmModeFreeObjectProperties(props);
  return type;
}

// First OVERLAY plane on `pipe` that scans out `fourcc`, or nullopt.
std::optional<std::uint32_t> find_overlay_plane(int fd, int pipe, std::uint32_t fourcc) {
  auto* pres = drmModeGetPlaneResources(fd);
  if (pres == nullptr) {
    return std::nullopt;
  }
  std::optional<std::uint32_t> found;
  for (std::uint32_t i = 0; i < pres->count_planes && !found.has_value(); ++i) {
    auto* plane = drmModeGetPlane(fd, pres->planes[i]);
    if (plane == nullptr) {
      continue;
    }
    const bool on_pipe = (plane->possible_crtcs & (1U << static_cast<unsigned>(pipe))) != 0;
    bool has_fmt = false;
    for (std::uint32_t f = 0; f < plane->count_formats; ++f) {
      if (plane->formats[f] == fourcc) {
        has_fmt = true;
        break;
      }
    }
    const auto type = plane_type(fd, plane->plane_id);
    if (on_pipe && has_fmt && type.has_value() && *type == DRM_PLANE_TYPE_OVERLAY) {
      found = plane->plane_id;
    }
    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(pres);
  return found;
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

// A full-screen background layer keeps the primary plane occupied so the
// pinned overlay isn't the only plane the commit touches.
void add_background(SceneFixture& fx, std::uint32_t w, std::uint32_t h) {
  auto src = DumbBufferSource::create(*fx.dev, w, h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(src.has_value()) << src.error().message();
  LayerDesc bg;
  bg.source = std::move(*src);
  bg.display.src_rect = drm::scene::Rect{0, 0, w, h};
  bg.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  bg.display.zpos = 0;
  ASSERT_TRUE(fx.scene->add_layer(std::move(bg)).has_value());
}

}  // namespace

TEST(LayerScenePinVkms, PinnedLayerLandsOnItsPlane) {
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

  const auto overlay = find_overlay_plane(fx.dev->fd(), fx.active.pipe, DRM_FORMAT_ARGB8888);
  if (!overlay) {
    cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
    GTEST_SKIP() << "no ARGB8888-capable overlay plane on this CRTC to pin to";
  }

  add_background(fx, fb_w, fb_h);

  const std::uint32_t cw = fb_w / 2;
  const std::uint32_t ch = fb_h / 2;
  auto src = DumbBufferSource::create(*fx.dev, cw, ch, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(src.has_value()) << src.error().message();
  LayerDesc pinned;
  pinned.source = std::move(*src);
  pinned.display.src_rect = drm::scene::Rect{0, 0, cw, ch};
  pinned.display.dst_rect = drm::scene::Rect{0, 0, cw, ch};
  pinned.display.zpos = 1;
  pinned.pinned_plane_id = *overlay;
  auto handle_r = fx.scene->add_layer(std::move(pinned));
  ASSERT_TRUE(handle_r.has_value()) << handle_r.error().message();
  const auto handle = *handle_r;

  auto report = fx.scene->commit();
  ASSERT_TRUE(report.has_value()) << report.error().message();
  EXPECT_EQ(report->pins_failed, 0U);

  auto* layer = fx.scene->get_layer(handle);
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(layer->last_placement(), LayerPlacement::AssignedToPlane)
      << "a pinned layer must reach scanout on its own plane";
  ASSERT_TRUE(layer->last_assigned_plane_id().has_value());
  EXPECT_EQ(*layer->last_assigned_plane_id(), *overlay)
      << "the pinned layer must land on exactly the plane it was pinned to";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

TEST(LayerScenePinVkms, UnhonorablePinFallsBackAndCountsFailure) {
  const auto node = find_scene_card();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  // Pin to a plane id that does not exist on this CRTC. The pin can't be
  // honored, so the layer must fall back to normal allocation (still
  // reach scanout) while pins_failed records the violation.
  auto src = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(src.has_value()) << src.error().message();
  LayerDesc d;
  d.source = std::move(*src);
  d.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  d.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  d.display.zpos = 0;
  d.pinned_plane_id = std::uint32_t{0xFFFFFFFFU};  // no such plane
  auto handle_r = fx.scene->add_layer(std::move(d));
  ASSERT_TRUE(handle_r.has_value()) << handle_r.error().message();
  const auto handle = *handle_r;

  auto report = fx.scene->commit();
  ASSERT_TRUE(report.has_value()) << report.error().message();
  EXPECT_GE(report->pins_failed, 1U) << "an un-honorable pin must be counted, not silent";

  auto* layer = fx.scene->get_layer(handle);
  ASSERT_NE(layer, nullptr);
  EXPECT_NE(layer->last_placement(), LayerPlacement::Unassigned)
      << "a failed pin must fall back to normal allocation, not drop the layer";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}
