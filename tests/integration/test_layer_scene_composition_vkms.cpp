// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration test for LayerScene's Phase 2.3 composition fallback,
// against the kernel's virtual KMS driver (VKMS).
//
// What it proves:
//   1. A `LayerDesc` with `force_composited = true` round-trips through
//      the allocator marked for composition (`needs_composition()`),
//      gets blended into the scene's `CompositeCanvas` by
//      `compose_unassigned()`, and the canvas is armed onto a free
//      hardware plane via direct property writes.
//   2. The committed plane composition (background plane + canvas
//      plane) reads back via `drm::capture::snapshot()` with the
//      composited layer's pixels visible at its `dst_rect` and
//      untouched outside it.
//   3. `CommitReport` reflects the rescue: 1 hardware-assigned, 1
//      composited, 0 dropped.
//
// Preconditions (mirrors test_capture_vkms.cpp):
//   - VKMS module loaded with overlay support:
//       sudo modprobe vkms enable_overlay=1
//   - Read/write access to /dev/dri/card* — a fresh open() on the
//     VKMS node makes the test the DRM master for that device.
//
// If VKMS is not loaded the test self-skips via GTEST_SKIP() so the
// suite stays green on developer machines that haven't modprobed it.

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/capture/snapshot.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
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
using drm::capture::Image;
using drm::capture::snapshot;
using drm::scene::DumbBufferSource;
using drm::scene::LayerDesc;
using drm::scene::LayerScene;

namespace {

// Locate /dev/dri/cardN for the VKMS driver, if present. Mirrors the
// helper in test_capture_vkms.cpp; a third caller would justify hoisting
// it into a tests/integration/vkms_helpers.hpp shared header.
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

// Fill `source`'s pixels with a uniform ARGB8888 value, accounting for
// any stride padding the kernel inserted.
void fill_uniform_argb(DumbBufferSource& source, std::uint32_t width, std::uint32_t height,
                       std::uint32_t pixel) {
  auto mapping = source.map(drm::MapAccess::Write);
  ASSERT_TRUE(mapping.has_value());
  const auto pixels = mapping->pixels();
  const std::uint32_t stride = mapping->stride();
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row = reinterpret_cast<std::uint32_t*>(pixels.data() + (y * stride));
    for (std::uint32_t x = 0; x < width; ++x) {
      row[x] = pixel;
    }
  }
}

}  // namespace

TEST(LayerSceneCompositionVkms, ForceCompositedLayerLandsOnCanvas) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded — `sudo modprobe vkms enable_overlay=1` "
                    "to enable this test";
  }

  auto dev_r = Device::open(*node);
  ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
  auto& dev = *dev_r;
  ASSERT_TRUE(dev.enable_universal_planes().has_value());
  ASSERT_TRUE(dev.enable_atomic().has_value());

  const auto active_r = pick_crtc(dev.fd());
  ASSERT_TRUE(active_r.has_value()) << active_r.error().message();
  const auto& active = *active_r;
  const std::uint32_t fb_w = active.mode.hdisplay;
  const std::uint32_t fb_h = active.mode.vdisplay;
  ASSERT_GE(fb_w, 64U);
  ASSERT_GE(fb_h, 64U);

  // Background source: full-screen opaque red. Goes through the
  // allocator, expected to land on PRIMARY.
  auto bg_source = DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg_source.has_value()) << bg_source.error().message();
  fill_uniform_argb(**bg_source, fb_w, fb_h, 0xFFFF0000U);  // opaque red

  // Overlay source: small opaque green block. force_composited routes
  // it through compose_unassigned() unconditionally — proves the path
  // even when the allocator could otherwise have placed it.
  const std::uint32_t overlay_w = fb_w / 4U;
  const std::uint32_t overlay_h = fb_h / 4U;
  const std::int32_t overlay_x = static_cast<std::int32_t>(fb_w / 4U);
  const std::int32_t overlay_y = static_cast<std::int32_t>(fb_h / 4U);
  auto overlay_source = DumbBufferSource::create(dev, overlay_w, overlay_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(overlay_source.has_value()) << overlay_source.error().message();
  fill_uniform_argb(**overlay_source, overlay_w, overlay_h, 0xFF00FF00U);  // opaque green

  LayerScene::Config cfg;
  cfg.crtc_id = active.crtc_id;
  cfg.connector_id = active.connector_id;
  cfg.mode = active.mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  // Layer A: background. zpos low so it sits below the overlay.
  LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_source);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.zpos = 1;
  auto bg_handle = scene.add_layer(std::move(bg_desc));
  ASSERT_TRUE(bg_handle.has_value()) << bg_handle.error().message();

  // Layer B: force_composited. zpos higher than bg so SRC_OVER paints
  // it on top of the canvas's transparent background — the canvas
  // itself sits above the bg plane via its own zpos pick.
  LayerDesc overlay_desc;
  overlay_desc.source = std::move(*overlay_source);
  overlay_desc.display.src_rect = drm::scene::Rect{0, 0, overlay_w, overlay_h};
  overlay_desc.display.dst_rect = drm::scene::Rect{overlay_x, overlay_y, overlay_w, overlay_h};
  overlay_desc.display.zpos = 4;  // >= 3 to clear amdgpu PRIMARY pin (2); harmless on VKMS
  overlay_desc.force_composited = true;
  auto overlay_handle = scene.add_layer(std::move(overlay_desc));
  ASSERT_TRUE(overlay_handle.has_value()) << overlay_handle.error().message();

  auto report_r = scene.commit();
  ASSERT_TRUE(report_r.has_value()) << report_r.error().message();
  const auto& report = *report_r;

  // CommitReport invariants — the report shape is the contract
  // compose_unassigned() promises.
  EXPECT_EQ(report.layers_total, 2U);
  EXPECT_EQ(report.layers_assigned, 1U) << "background should fit on a hardware plane";
  EXPECT_EQ(report.layers_composited, 1U) << "force_composited overlay should be rescued";
  EXPECT_EQ(report.layers_unassigned, 0U) << "no layer should drop";
  EXPECT_EQ(report.composition_buckets, 1U);

  auto img_r = snapshot(dev, active.crtc_id);
  // Tear down the CRTC binding before any failures to avoid leaving an
  // active FB across the test boundary.
  drmModeSetCrtc(dev.fd(), active.crtc_id, 0, 0, 0, nullptr, 0, nullptr);
  ASSERT_TRUE(img_r.has_value()) << img_r.error().message();
  const Image img = std::move(*img_r);
  ASSERT_EQ(img.width(), fb_w);
  ASSERT_EQ(img.height(), fb_h);

  auto at = [&](std::uint32_t x, std::uint32_t y) { return img.pixels()[(y * img.width()) + x]; };

  // Inside the overlay's dst_rect: green (composited canvas, opaque
  // green pixel SRC_OVER'd onto a transparent canvas, then composed
  // over the red background plane → green wins because src_a == 0xFF).
  const std::uint32_t inside_x = static_cast<std::uint32_t>(overlay_x) + (overlay_w / 2U);
  const std::uint32_t inside_y = static_cast<std::uint32_t>(overlay_y) + (overlay_h / 2U);
  EXPECT_EQ(at(inside_x, inside_y), 0xFF00FF00U)
      << "centre of overlay rect should be the composited green pixel";

  // Outside the overlay's dst_rect: red (background plane only). Sample
  // the four extreme corners of the framebuffer — every corner is well
  // outside the centred quarter-screen overlay.
  EXPECT_EQ(at(0, 0), 0xFFFF0000U) << "top-left should be background red";
  EXPECT_EQ(at(fb_w - 1U, 0), 0xFFFF0000U) << "top-right should be background red";
  EXPECT_EQ(at(0, fb_h - 1U), 0xFFFF0000U) << "bottom-left should be background red";
  EXPECT_EQ(at(fb_w - 1U, fb_h - 1U), 0xFFFF0000U) << "bottom-right should be background red";
}
