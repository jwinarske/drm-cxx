// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// A cursor driven on an OVERLAY plane near the CRTC's top-left corner: its
// position goes negative once the hotspot is subtracted, and some drivers
// refuse a negative CRTC_X/CRTC_Y. The renderer clips the plane at the edge
// and trims the source to match; this reads the plane back to check.
//
// Runs against VKMS (`sudo modprobe vkms enable_overlay=1`) and self-skips
// when it isn't loaded. Set DRM_CXX_TEST_CARD=/dev/dri/cardN for real
// hardware. Requires DRM master.

#include "vkms_node.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/cursor/cursor.hpp>
#include <drm-cxx/cursor/renderer.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/planes/plane_registry.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <utility>
#include <vector>

namespace {

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

struct Output {
  std::uint32_t crtc_id{0};
  std::uint32_t crtc_index{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

// First connected connector and a CRTC its first encoder can drive.
std::optional<Output> pick_output(int fd) {
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return std::nullopt;
  }
  std::optional<Output> out;
  for (int i = 0; i < res->count_connectors && !out.has_value(); ++i) {
    auto* conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn == nullptr) {
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 &&
        conn->count_encoders > 0) {
      if (auto* enc = drmModeGetEncoder(fd, conn->encoders[0]); enc != nullptr) {
        for (int c = 0; c < res->count_crtcs; ++c) {
          if ((enc->possible_crtcs & (1U << static_cast<unsigned>(c))) != 0) {
            out = Output{res->crtcs[c], static_cast<std::uint32_t>(c), conn->connector_id,
                         conn->modes[0]};
            break;
          }
        }
        drmModeFreeEncoder(enc);
      }
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);
  return out;
}

}  // namespace

TEST(CursorOverlayClipVkms, CursorAtTheTopLeftIsClipped) {
  const auto node = drm::test::find_test_card_or_vkms();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto dev = drm::Device::open(*node);
  ASSERT_TRUE(dev.has_value()) << dev.error().message();
  ASSERT_TRUE(dev->enable_universal_planes().has_value());
  ASSERT_TRUE(dev->enable_atomic().has_value());
  const int fd = dev->fd();
  const auto out = pick_output(fd);
  ASSERT_TRUE(out.has_value());

  auto registry = drm::planes::PlaneRegistry::enumerate(*dev);
  ASSERT_TRUE(registry.has_value());
  std::optional<std::uint32_t> overlay;
  for (const auto* cap : registry->for_crtc(out->crtc_index)) {
    if (cap->type == drm::planes::DRMPlaneType::OVERLAY &&
        cap->scans_out_linear(DRM_FORMAT_ARGB8888)) {
      overlay = cap->id;
    }
  }
  if (!overlay) {
    GTEST_SKIP() << "no ARGB8888 overlay plane on this CRTC";
  }

  // Light the CRTC with a full-screen background on the primary.
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = out->crtc_id;
  cfg.connector_id = out->connector_id;
  cfg.mode = out->mode;
  auto scene = drm::scene::LayerScene::create(*dev, cfg);
  ASSERT_TRUE(scene.has_value()) << scene.error().message();
  const std::uint32_t w = out->mode.hdisplay;
  const std::uint32_t h = out->mode.vdisplay;
  auto bg = drm::scene::DumbBufferSource::create(*dev, w, h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg.has_value()) << bg.error().message();
  drm::scene::LayerDesc desc;
  desc.source = std::move(*bg);
  desc.display.src_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.zpos = 0;
  ASSERT_TRUE((*scene)->add_layer(std::move(desc)).has_value());
  ASSERT_TRUE((*scene)->commit().has_value());

  drm::cursor::RendererConfig rcfg;
  rcfg.crtc_id = out->crtc_id;
  rcfg.forced_plane_id = *overlay;
  rcfg.preferred_size = 64;
  auto renderer = drm::cursor::Renderer::create(*dev, rcfg);
  ASSERT_TRUE(renderer.has_value()) << renderer.error().message();
  const std::vector<std::uint32_t> pixels(32U * 32U, 0xFFFFFFFFU);
  auto cursor = drm::cursor::Cursor::from_argb(
      drm::span<const std::uint32_t>(pixels.data(), pixels.size()), 32, 32, 8, 8);
  ASSERT_TRUE(cursor.has_value()) << cursor.error().message();
  ASSERT_TRUE(renderer->set_cursor(std::move(*cursor)).has_value());

  // The 32x32 sprite sits centered in the 64x64 buffer, so the plane origin
  // is (pointer - 16 - hotspot): (-24, -24) at the CRTC's corner.
  auto moved = renderer->move_to(0, 0);
  ASSERT_TRUE(moved.has_value()) << moved.error().message();

  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_X"), 0U);
  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_Y"), 0U);
  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_W"), 40U);
  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_H"), 40U);
  EXPECT_EQ(plane_prop(fd, *overlay, "SRC_X"), 24U << 16U);
  EXPECT_EQ(plane_prop(fd, *overlay, "SRC_Y"), 24U << 16U);
  EXPECT_EQ(plane_prop(fd, *overlay, "SRC_W"), 40U << 16U);
  EXPECT_EQ(plane_prop(fd, *overlay, "SRC_H"), 40U << 16U);

  drmModeSetCrtc(fd, out->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}
