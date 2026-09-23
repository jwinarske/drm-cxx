// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// DisplayParams::src_rect_fixed against a live KMS CRTC: a 16.16 source rect
// reaches the plane's SRC_* properties as given, so a sub-pixel crop (a Wayland
// viewport source rect, for one) is not rounded to whole pixels on the way.
//
// Runs against VKMS by default and self-skips when it isn't loaded. Set
// DRM_CXX_TEST_CARD=/dev/dri/cardN to run against real hardware instead.
// Requires DRM master.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/display_params.hpp>
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

// The value the kernel holds for a plane property, or nullopt.
std::optional<std::uint64_t> plane_prop(int fd, std::uint32_t plane_id, const char* name) {
  auto* props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (props == nullptr) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> value;
  for (std::uint32_t i = 0; i < props->count_props; ++i) {
    auto* prop = drmModeGetProperty(fd, props->props[i]);
    if (prop != nullptr) {
      if (std::strcmp(prop->name, name) == 0) {
        value = props->prop_values[i];
      }
      drmModeFreeProperty(prop);
    }
  }
  drmModeFreeObjectProperties(props);
  return value;
}

}  // namespace

// A crop that starts half a pixel in, the size of the destination: no scaling,
// so any plane can take it, and SRC_X must read back as 0.5 rather than 0.
TEST(LayerSceneSrcFixedVkms, SubPixelCropReachesTheKernelUnrounded) {
  const auto node = find_scene_card();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded — `sudo modprobe vkms` to enable this test";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  const std::uint32_t w = fx.active.mode.hdisplay;
  const std::uint32_t h = fx.active.mode.vdisplay;

  // One column wider than the crop, so the half-pixel offset stays inside it.
  auto src = DumbBufferSource::create(*fx.dev, w + 1, h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(src.has_value()) << src.error().message();
  LayerDesc desc;
  desc.source = std::move(*src);
  desc.display.src_rect_fixed = drm::scene::FixedRect{0x8000U, 0, w << 16U, h << 16U};
  desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  auto handle_r = fx.scene->add_layer(std::move(desc));
  ASSERT_TRUE(handle_r.has_value()) << handle_r.error().message();

  auto report = fx.scene->commit();
  ASSERT_TRUE(report.has_value()) << report.error().message();
  auto* layer = fx.scene->get_layer(*handle_r);
  ASSERT_NE(layer, nullptr);
  ASSERT_EQ(layer->last_placement(), LayerPlacement::AssignedToPlane);
  const auto assigned = layer->last_assigned_plane_id();
  ASSERT_TRUE(assigned.has_value());
  const std::uint32_t plane = assigned.value_or(0U);

  EXPECT_EQ(plane_prop(fx.dev->fd(), plane, "SRC_X"), std::optional<std::uint64_t>{0x8000U});
  EXPECT_EQ(plane_prop(fx.dev->fd(), plane, "SRC_Y"), std::optional<std::uint64_t>{0U});
  EXPECT_EQ(plane_prop(fx.dev->fd(), plane, "SRC_W"),
            std::optional<std::uint64_t>{static_cast<std::uint64_t>(w) << 16U});
  EXPECT_EQ(plane_prop(fx.dev->fd(), plane, "SRC_H"),
            std::optional<std::uint64_t>{static_cast<std::uint64_t>(h) << 16U});

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}
