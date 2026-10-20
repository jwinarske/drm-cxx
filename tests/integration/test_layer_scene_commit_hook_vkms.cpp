// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// LayerScene::set_commit_hook: a cursor on an OVERLAY plane the scene leaves
// alone (set_external_reserved_planes) changes in the scene's own commit,
// staged by the hook, instead of in a commit of its own. Reads the plane back
// to check, and that test() never runs the hook.
//
// Runs against VKMS (`sudo modprobe vkms enable_overlay=1`) and self-skips
// when it isn't loaded. Set DRM_CXX_TEST_CARD=/dev/dri/cardN for real
// hardware. Requires DRM master.

#include "vkms_node.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/cursor/cursor.hpp>
#include <drm-cxx/cursor/renderer.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/mode_list.hpp>
#include <drm-cxx/planes/plane_registry.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <system_error>
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
// DRM_CXX_CONNECTOR=<name> ("DP-4") picks the output: shared-display
// controllers report several connected but unlit outputs.
std::optional<Output> pick_output(int fd) {
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return std::nullopt;
  }
  const char* want = std::getenv("DRM_CXX_CONNECTOR");
  std::optional<Output> out;
  for (int i = 0; i < res->count_connectors && !out.has_value(); ++i) {
    auto* conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn == nullptr) {
      continue;
    }
    const bool wanted = want == nullptr || *want == '\0' ||
                        std::string(drm::display::connector_type_name(conn->connector_type)) + "-" +
                                std::to_string(conn->connector_type_id) ==
                            want;
    if (wanted && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 &&
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

TEST(LayerSceneCommitHookVkms, CursorRidesTheSceneCommit) {
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
    if (cap->type == drm::planes::DRMPlaneType::OVERLAY && !cap->multirect_parent.has_value() &&
        cap->scans_out_linear(DRM_FORMAT_ARGB8888) && !overlay.has_value()) {
      overlay = cap->id;
    }
  }
  if (!overlay) {
    GTEST_SKIP() << "no ARGB8888 overlay plane on this CRTC";
  }

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

  const std::uint32_t reserved[] = {*overlay};
  (*scene)->set_external_reserved_planes(drm::span<const std::uint32_t>(reserved, 1));
  int pointer_x = 100;
  int pointer_y = 100;
  int calls = 0;
  std::error_code fail;
  (*scene)->set_commit_hook([&](drm::AtomicRequest& req) -> drm::expected<void, std::error_code> {
    ++calls;
    if (fail) {
      return drm::unexpected<std::error_code>(fail);
    }
    bool first = false;
    auto staged = renderer->stage(req, pointer_x, pointer_y, first);
    if (!staged) {
      return drm::unexpected<std::error_code>(staged.error());
    }
    return {};
  });

  // test() leaves the hook alone.
  ASSERT_TRUE((*scene)->test().has_value());
  EXPECT_EQ(calls, 0);

  // The scene's first commit lights the CRTC and brings the cursor up with
  // it. The 32x32 sprite sits centered in the 64x64 buffer: the plane origin
  // is the pointer less 16, less the hotspot.
  ASSERT_TRUE((*scene)->commit().has_value());
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_ID"), out->crtc_id);
  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_X"), 76U);
  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_Y"), 76U);

  // A later scene commit moves it.
  pointer_x = 200;
  pointer_y = 150;
  ASSERT_TRUE((*scene)->commit().has_value());
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_X"), 176U);
  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_Y"), 126U);

  // A hook error fails the commit with it, and nothing changes.
  fail = std::make_error_code(std::errc::io_error);
  auto failed = (*scene)->commit();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error(), fail);
  EXPECT_EQ(plane_prop(fd, *overlay, "CRTC_X"), 176U);

  // The scene commits on without a hook.
  (*scene)->set_commit_hook({});
  ASSERT_TRUE((*scene)->commit().has_value());
  EXPECT_EQ(calls, 3);

  drmModeSetCrtc(fd, out->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}
