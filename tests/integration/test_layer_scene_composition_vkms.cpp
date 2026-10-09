// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration test for LayerScene's composition fallback,
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

#include "vkms_node.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/capture/snapshot.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/scene/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using drm::Device;
using drm::capture::Image;
using drm::capture::snapshot;
using drm::scene::DumbBufferSource;
using drm::scene::LayerDesc;
using drm::scene::LayerScene;

namespace {

// The DMA-BUF-import rescue test needs a GL-capable card. Honor an explicit
// DRM_CXX_TEST_CARD override (a real GPU card, e.g. vc4 on a Pi) so the GPU
// import path can be exercised on hardware; else fall back to vkms.
std::optional<std::string> find_dmabuf_import_card() {
  if (const char* node = std::getenv("DRM_CXX_TEST_CARD"); node != nullptr && *node != '\0') {
    return std::string(node);
  }
  return drm::test::find_vkms_node();
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

// Non-cursor planes that can scan out on `crtc_id`.
std::uint32_t scene_plane_count(int fd, std::uint32_t crtc_id) {
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return 0;
  }
  std::uint32_t crtc_bit = 0;
  for (int c = 0; c < res->count_crtcs; ++c) {
    if (res->crtcs[c] == crtc_id) {
      crtc_bit = 1U << static_cast<unsigned>(c);
    }
  }
  drmModeFreeResources(res);
  auto* planes = drmModeGetPlaneResources(fd);
  if (planes == nullptr) {
    return 0;
  }
  std::uint32_t count = 0;
  for (std::uint32_t i = 0; i < planes->count_planes; ++i) {
    auto* p = drmModeGetPlane(fd, planes->planes[i]);
    const bool on_crtc = p != nullptr && (p->possible_crtcs & crtc_bit) != 0;
    drmModeFreePlane(p);
    if (!on_crtc) {
      continue;
    }
    auto* props = drmModeObjectGetProperties(fd, planes->planes[i], DRM_MODE_OBJECT_PLANE);
    bool cursor = false;
    for (std::uint32_t k = 0; props != nullptr && k < props->count_props; ++k) {
      auto* prop = drmModeGetProperty(fd, props->props[k]);
      if (prop != nullptr && std::strcmp(prop->name, "type") == 0) {
        cursor = props->prop_values[k] == DRM_PLANE_TYPE_CURSOR;
      }
      drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    count += cursor ? 0U : 1U;
  }
  drmModeFreePlaneResources(planes);
  return count;
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
  const auto node = drm::test::find_vkms_node();
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
      << "center of overlay rect should be the composited green pixel";

  // Outside the overlay's dst_rect: red (background plane only). Sample
  // the four extreme corners of the framebuffer — every corner is well
  // outside the centerd quarter-screen overlay.
  EXPECT_EQ(at(0, 0), 0xFFFF0000U) << "top-left should be background red";
  EXPECT_EQ(at(fb_w - 1U, 0), 0xFFFF0000U) << "top-right should be background red";
  EXPECT_EQ(at(0, fb_h - 1U), 0xFFFF0000U) << "bottom-left should be background red";
  EXPECT_EQ(at(fb_w - 1U, fb_h - 1U), 0xFFFF0000U) << "bottom-right should be background red";
}

// When composition stops, the canvas plane must go dark. The allocator only
// disables planes it armed itself, so without the scene's own disable the old
// canvas frame stayed on screen after the composited layer was removed.
TEST(LayerSceneCompositionVkms, CanvasPlaneTurnsOffWhenCompositionStops) {
  const auto node = drm::test::find_vkms_node();
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

  auto bg_source = DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg_source.has_value()) << bg_source.error().message();
  fill_uniform_argb(**bg_source, fb_w, fb_h, 0xFFFF0000U);  // opaque red
  const std::uint32_t ow = fb_w / 4U;
  const std::uint32_t oh = fb_h / 4U;
  const auto ox = static_cast<std::int32_t>(fb_w / 4U);
  const auto oy = static_cast<std::int32_t>(fb_h / 4U);
  auto ov_source = DumbBufferSource::create(dev, ow, oh, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(ov_source.has_value()) << ov_source.error().message();
  fill_uniform_argb(**ov_source, ow, oh, 0xFF00FF00U);  // opaque green

  LayerScene::Config cfg;
  cfg.crtc_id = active.crtc_id;
  cfg.connector_id = active.connector_id;
  cfg.mode = active.mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_source);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.zpos = 1;
  ASSERT_TRUE(scene.add_layer(std::move(bg_desc)).has_value());
  LayerDesc ov_desc;
  ov_desc.source = std::move(*ov_source);
  ov_desc.display.src_rect = drm::scene::Rect{0, 0, ow, oh};
  ov_desc.display.dst_rect = drm::scene::Rect{ox, oy, ow, oh};
  ov_desc.display.zpos = 4;
  ov_desc.force_composited = true;
  auto ov_handle = scene.add_layer(std::move(ov_desc));
  ASSERT_TRUE(ov_handle.has_value()) << ov_handle.error().message();

  auto first = scene.commit();
  ASSERT_TRUE(first.has_value()) << first.error().message();
  ASSERT_EQ(first->layers_composited, 1U);

  scene.remove_layer(*ov_handle);
  auto second = scene.commit();
  ASSERT_TRUE(second.has_value()) << second.error().message();
  EXPECT_EQ(second->layers_composited, 0U);

  auto img_r = snapshot(dev, active.crtc_id);
  drmModeSetCrtc(dev.fd(), active.crtc_id, 0, 0, 0, nullptr, 0, nullptr);
  ASSERT_TRUE(img_r.has_value()) << img_r.error().message();
  const Image img = std::move(*img_r);
  const auto cx = static_cast<std::uint32_t>(ox) + (ow / 2U);
  const auto cy = static_cast<std::uint32_t>(oy) + (oh / 2U);
  EXPECT_EQ(img.pixels()[(cy * img.width()) + cx], 0xFFFF0000U)
      << "the removed overlay's area must show the background, not the stale canvas";
}

// A map()-less source (no CPU pixels) that can export its dma-buf must be
// rescued by GPU composition — imported as an EGLImage — rather than dropped,
// when the composition target is a GlCompositor. Self-skips when the scene
// falls back to the CPU CompositeCanvas (no GL on this card), which cannot
// import a dma-buf and correctly drops the layer.
TEST(LayerSceneCompositionVkms, DmaBufSourceRescuedByGpuImport) {
  const auto node = find_dmabuf_import_card();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a GL-capable card)";
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

  // CPU background that lands on a plane.
  auto bg = DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg.has_value()) << bg.error().message();
  fill_uniform_argb(**bg, fb_w, fb_h, 0xFFFF0000U);

  // A map()-less overlay backed by a dma-buf (gbm bo), force_composited: the
  // only way it can reach the canvas is the GPU EGLImage import path.
  const std::uint32_t ow = fb_w / 4U;
  const std::uint32_t oh = fb_h / 4U;
  gbm_device* gbm = gbm_create_device(dev.fd());
  ASSERT_NE(gbm, nullptr);
  // LINEAR (not RENDERING) so the modifier is DRM_FORMAT_MOD_LINEAR, which
  // ExternalDmaBufSource requires; a RENDERING bo on some drivers (vc4) is
  // tiled and would be rejected. EGL imports the linear dma-buf regardless.
  gbm_bo* bo =
      gbm_bo_create(gbm, ow, oh, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT);
  if (bo == nullptr) {
    gbm_device_destroy(gbm);
    GTEST_SKIP() << "gbm_bo_create(ARGB, LINEAR) unsupported on this stack";
  }
  void* md = nullptr;
  std::uint32_t bo_stride = 0;
  void* p = gbm_bo_map(bo, 0, 0, ow, oh, GBM_BO_TRANSFER_WRITE, &bo_stride, &md);
  ASSERT_NE(p, nullptr);
  auto* px = static_cast<std::uint8_t*>(p);
  for (std::uint32_t y = 0; y < oh; ++y) {
    for (std::uint32_t x = 0; x < ow; ++x) {
      std::uint8_t* q = px + (static_cast<std::size_t>(y) * bo_stride) + (x * 4U);
      q[0] = 0x00U;
      q[1] = 0xFFU;
      q[2] = 0x00U;
      q[3] = 0xFFU;  // opaque green (B,G,R,A)
    }
  }
  gbm_bo_unmap(bo, md);

  const int fd = gbm_bo_get_fd(bo);
  ASSERT_GE(fd, 0);
  const std::array<drm::scene::ExternalPlaneInfo, 1> planes{
      {drm::scene::ExternalPlaneInfo{fd, 0, gbm_bo_get_stride(bo)}}};
  auto src = drm::scene::ExternalDmaBufSource::create(
      dev, ow, oh, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
      drm::span<const drm::scene::ExternalPlaneInfo>(planes.data(), planes.size()));
  ::close(fd);  // the source dup'd it
  ASSERT_TRUE(src.has_value()) << src.error().message();

  LayerScene::Config cfg;
  cfg.crtc_id = active.crtc_id;
  cfg.connector_id = active.connector_id;
  cfg.mode = active.mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  LayerDesc bg_desc;
  bg_desc.source = std::move(*bg);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.zpos = 1;
  ASSERT_TRUE(scene.add_layer(std::move(bg_desc)).has_value());

  LayerDesc od;
  od.source = std::move(*src);
  od.display.src_rect = drm::scene::Rect{0, 0, ow, oh};
  od.display.dst_rect =
      drm::scene::Rect{static_cast<std::int32_t>(ow), static_cast<std::int32_t>(oh), ow, oh};
  od.display.zpos = 4;
  od.force_composited = true;
  auto handle_r = scene.add_layer(std::move(od));
  ASSERT_TRUE(handle_r.has_value()) << handle_r.error().message();
  const auto handle = *handle_r;

  auto report_r = scene.commit();
  ASSERT_TRUE(report_r.has_value()) << report_r.error().message();
  auto* layer = scene.get_layer(handle);
  ASSERT_NE(layer, nullptr);

  const bool composited = layer->last_placement() == drm::scene::LayerPlacement::Composited;
  if (composited) {
    EXPECT_GE(report_r->layers_composited, 1U)
        << "the map()-less dma-buf source was imported and composited";
  } else {
    // CPU composition target (no GL) can't import — the layer is correctly
    // dropped and the import path isn't exercised here.
    EXPECT_EQ(layer->last_placement(), drm::scene::LayerPlacement::Unassigned);
  }

  gbm_bo_destroy(bo);
  gbm_device_destroy(gbm);
  if (!composited) {
    GTEST_SKIP() << "composition target is CPU-only (no GL) — dma-buf import path not exercised";
  }
}

// Regression: when full_search exhausts its TEST budget without finding
// any non-empty assignment (every layer falls through to compose_unassigned),
// the next frame's fast path must NOT return EAGAIN. The cold-start path
// used to mark previous_allocation_valid_=true even when best_assignment
// was empty; the warm-start fast path then hit its empty-map guard and
// propagated EAGAIN to the caller, killing examples that rely on full
// composition fallback (notably scene_formats on Granite Ridge amdgpu —
// `assigned=0 composited=4` on frame 1 followed by EAGAIN on frame 2).
//
// The test forces empty cold-start by marking every layer
// `force_composited=true`, which makes plane_statically_compatible reject
// every (plane, layer) pair. full_search then walks preseed → greedy →
// backtracking and exits with best_assignment empty.
TEST(LayerSceneCompositionVkms, EmptyColdStartDoesNotPoisonWarmStart) {
  const auto node = drm::test::find_vkms_node();
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

  auto bg_source = DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(bg_source.has_value()) << bg_source.error().message();
  fill_uniform_argb(**bg_source, fb_w, fb_h, 0xFFFF0000U);

  const std::uint32_t overlay_w = fb_w / 4U;
  const std::uint32_t overlay_h = fb_h / 4U;
  auto overlay_source = DumbBufferSource::create(dev, overlay_w, overlay_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(overlay_source.has_value()) << overlay_source.error().message();
  fill_uniform_argb(**overlay_source, overlay_w, overlay_h, 0xFF00FF00U);

  LayerScene::Config cfg;
  cfg.crtc_id = active.crtc_id;
  cfg.connector_id = active.connector_id;
  cfg.mode = active.mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_source);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.zpos = 1;
  bg_desc.force_composited = true;
  ASSERT_TRUE(scene.add_layer(std::move(bg_desc)).has_value());

  LayerDesc overlay_desc;
  overlay_desc.source = std::move(*overlay_source);
  overlay_desc.display.src_rect = drm::scene::Rect{0, 0, overlay_w, overlay_h};
  overlay_desc.display.dst_rect =
      drm::scene::Rect{static_cast<std::int32_t>(fb_w / 4U), static_cast<std::int32_t>(fb_h / 4U),
                       overlay_w, overlay_h};
  overlay_desc.display.zpos = 4;
  overlay_desc.force_composited = true;
  ASSERT_TRUE(scene.add_layer(std::move(overlay_desc)).has_value());

  // Frame 1: cold-start. full_search returns 0 placed; both layers are
  // rescued by compose_unassigned onto the canvas plane.
  auto report1 = scene.commit();
  ASSERT_TRUE(report1.has_value()) << report1.error().message();
  EXPECT_EQ(report1->layers_total, 2U);
  EXPECT_EQ(report1->layers_assigned, 0U);
  EXPECT_EQ(report1->layers_composited, 2U);
  EXPECT_EQ(report1->layers_unassigned, 0U);

  // Frame 2: warm-start fast path. With the bug, this returns EAGAIN
  // (Resource temporarily unavailable) because the empty
  // previous_allocation_ trips the empty-map guard while
  // previous_allocation_valid_ is still true. With the fix, valid_ is
  // false → fast path skipped → full_search re-runs → succeeds with
  // the same shape as frame 1.
  auto report2 = scene.commit();
  ASSERT_TRUE(report2.has_value())
      << "second commit returned " << report2.error().message()
      << " — empty cold-start should not poison the warm-start fast path";
  EXPECT_EQ(report2->layers_assigned, 0U);
  EXPECT_EQ(report2->layers_composited, 2U);
  EXPECT_EQ(report2->layers_unassigned, 0U);

  drmModeSetCrtc(dev.fd(), active.crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}

// More layers than planes, all stacked on one spot. The canvas carries the
// layers the allocator dropped -- the top of the stack -- so the topmost
// layer's color must show. On a driver whose planes have no zpos property the
// plane order is the stacking order, so the canvas must sit on a plane above
// every plane carrying a layer.
TEST(LayerSceneCompositionVkms, CanvasStacksAboveAssignedLayers) {
  const auto node = drm::test::find_vkms_node();
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

  LayerScene::Config cfg;
  cfg.crtc_id = active.crtc_id;
  cfg.connector_id = active.connector_id;
  cfg.mode = active.mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  constexpr std::uint32_t k_layers = 16;  // more than vkms has planes
  constexpr std::uint32_t k_side = 64;
  const auto x = static_cast<std::int32_t>((fb_w - k_side) / 2U);
  const auto y = static_cast<std::int32_t>((fb_h - k_side) / 2U);
  auto color = [](std::uint32_t i) { return 0xFF000000U | ((i * 15U) << 16U) | 0x80U; };
  for (std::uint32_t i = 0; i < k_layers; ++i) {
    auto src = DumbBufferSource::create(dev, k_side, k_side, DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    fill_uniform_argb(**src, k_side, k_side, color(i));
    LayerDesc d;
    d.source = std::move(*src);
    d.display.src_rect = drm::scene::Rect{0, 0, k_side, k_side};
    d.display.dst_rect = drm::scene::Rect{x, y, k_side, k_side};
    d.display.zpos = static_cast<int>(i) + 3;
    ASSERT_TRUE(scene.add_layer(std::move(d)).has_value());
  }

  auto report = scene.commit();
  ASSERT_TRUE(report.has_value()) << report.error().message();
  ASSERT_GT(report->layers_composited, 0U) << "the stack should overflow into the canvas";
  EXPECT_EQ(report->layers_unassigned, 0U);

  auto img_r = snapshot(dev, active.crtc_id);
  drmModeSetCrtc(dev.fd(), active.crtc_id, 0, 0, 0, nullptr, 0, nullptr);
  ASSERT_TRUE(img_r.has_value()) << img_r.error().message();
  const auto cx = static_cast<std::uint32_t>(x) + (k_side / 2U);
  const auto cy = static_cast<std::uint32_t>(y) + (k_side / 2U);
  EXPECT_EQ(img_r->pixels()[(cy * img_r->width()) + cx], color(k_layers - 1U))
      << "the topmost layer, carried by the canvas, must be visible";
}

namespace {

bool is_vkms(int fd) {
  drmVersionPtr v = drmGetVersion(fd);
  const bool vkms = v != nullptr && v->name != nullptr && std::strcmp(v->name, "vkms") == 0;
  drmFreeVersion(v);
  return vkms;
}

// Stacks more overlapping layers than the CRTC has planes, `low(k, i)` marking
// the low-priority ones among k, and checks the composited layers form one
// zpos run and the top layer is what shows. `only_low` also requires every
// composited layer to be low priority.
void check_composited_stack(const std::function<bool(std::uint32_t, std::uint32_t)>& low,
                            bool only_low) {
  const auto node = drm::test::find_test_card_or_vkms();
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

  LayerScene::Config cfg;
  cfg.crtc_id = active.crtc_id;
  cfg.connector_id = active.connector_id;
  cfg.mode = active.mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  const std::uint32_t planes = scene_plane_count(dev.fd(), active.crtc_id);
  if (planes < 4U) {
    GTEST_SKIP() << "the CRTC has " << planes << " plane(s); the stack needs at least 4";
  }
  const std::uint32_t k_layers = planes + 4U;
  constexpr std::uint32_t k_side = 64;
  const auto x = static_cast<std::int32_t>((active.mode.hdisplay - k_side) / 2U);
  const auto y = static_cast<std::int32_t>((active.mode.vdisplay - k_side) / 2U);
  auto color = [](std::uint32_t i) { return 0xFF000000U | ((i * 11U) << 16U) | 0x80U; };
  std::vector<drm::scene::LayerHandle> handles;
  for (std::uint32_t i = 0; i < k_layers; ++i) {
    auto src = DumbBufferSource::create(dev, k_side, k_side, DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    fill_uniform_argb(**src, k_side, k_side, color(i));
    LayerDesc d;
    d.source = std::move(*src);
    d.display.src_rect = drm::scene::Rect{0, 0, k_side, k_side};
    d.display.dst_rect = drm::scene::Rect{x, y, k_side, k_side};
    d.display.zpos = static_cast<int>(i) + 3;
    d.app_priority = low(k_layers, i) ? 10 : 200;
    auto h = scene.add_layer(std::move(d));
    ASSERT_TRUE(h.has_value()) << h.error().message();
    handles.push_back(*h);
  }

  auto report = scene.commit();
  ASSERT_TRUE(report.has_value()) << report.error().message();
  ASSERT_GT(report->layers_composited, 0U) << "the stack should overflow into the canvas";
  EXPECT_EQ(report->layers_unassigned, 0U);
  std::optional<std::uint32_t> run_first;
  std::uint32_t run_last = 0;
  for (std::uint32_t i = 0; i < k_layers; ++i) {
    const auto* layer = scene.get_layer(handles[i]);
    ASSERT_NE(layer, nullptr);
    if (layer->last_placement() != drm::scene::LayerPlacement::AssignedToPlane) {
      EXPECT_TRUE(!only_low || low(k_layers, i))
          << "layer " << i << " (high priority) was composited";
      run_first = run_first.value_or(i);
      run_last = i;
    }
  }
  ASSERT_TRUE(run_first.has_value());
  EXPECT_EQ(run_last - *run_first + 1U, report->layers_composited)
      << "the composited layers must be one zpos run, layers " << *run_first << ".." << run_last;

  auto img_r = snapshot(dev, active.crtc_id);
  drmModeSetCrtc(dev.fd(), active.crtc_id, 0, 0, 0, nullptr, 0, nullptr);
  if (!img_r.has_value() && !is_vkms(dev.fd())) {
    GTEST_SKIP() << "no plane readback on this card: " << img_r.error().message();
  }
  ASSERT_TRUE(img_r.has_value()) << img_r.error().message();
  const auto cx = static_cast<std::uint32_t>(x) + (k_side / 2U);
  const auto cy = static_cast<std::uint32_t>(y) + (k_side / 2U);
  EXPECT_EQ(img_r->pixels()[(cy * img_r->width()) + cx], color(k_layers - 1U));
}

}  // namespace

// Plane pressure with the low-priority layers mid-stack. Only they may go to
// the canvas, and the stack must still render top-down: the composited run is
// contiguous, so the canvas can sit between its neighbors.
//
// Where planes take zpos, the canvas must stack under the placed layers above
// the run, not on top of them (#343). DRM_CXX_TEST_CARD runs it on such a card.
TEST(LayerSceneCompositionVkms, CompositedRunTakesLowPriorityLayers) {
  // The middle half (at least 6) is low priority, so the overflow plus the
  // canvas plane fits in it.
  auto low = [](std::uint32_t k, std::uint32_t i) {
    const std::uint32_t n = std::max(6U, k / 2U);
    const std::uint32_t first = (k - n) / 2U;
    return i >= first && i < first + n;
  };
  check_composited_stack(low, /*only_low=*/true);
}

// Low-priority layers at both ends of the stack, three each: compositing the
// lowest-priority ones would leave placed layers between them, on both sides
// of the one canvas. One contiguous run goes to the canvas instead.
TEST(LayerSceneCompositionVkms, SplitLowPriorityLayersCompositeOneRun) {
  auto low = [](std::uint32_t k, std::uint32_t i) {
    return (i >= 1U && i <= 3U) || (i >= k - 4U && i <= k - 2U);
  };
  check_composited_stack(low, /*only_low=*/false);
}

// Reversing the zpos of overlapping layers after steady frames must reach the
// screen. Where planes have no zpos property, the warm start's cached
// assignment is still valid to the kernel, just stacked in the old order.
TEST(LayerSceneCompositionVkms, RestackReachesTheScreen) {
  const auto node = drm::test::find_vkms_node();
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

  LayerScene::Config cfg;
  cfg.crtc_id = active.crtc_id;
  cfg.connector_id = active.connector_id;
  cfg.mode = active.mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  constexpr std::uint32_t k_side = 64;
  const auto x = static_cast<std::int32_t>((active.mode.hdisplay - k_side) / 2U);
  const auto y = static_cast<std::int32_t>((active.mode.vdisplay - k_side) / 2U);
  const std::array<std::uint32_t, 3> colors{0xFFFF0000U, 0xFF00FF00U, 0xFF0000FFU};
  std::vector<drm::scene::LayerHandle> handles;
  for (std::size_t i = 0; i < colors.size(); ++i) {
    auto src = DumbBufferSource::create(dev, k_side, k_side, DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    fill_uniform_argb(**src, k_side, k_side, colors.at(i));
    LayerDesc d;
    d.source = std::move(*src);
    d.display.src_rect = drm::scene::Rect{0, 0, k_side, k_side};
    d.display.dst_rect = drm::scene::Rect{x, y, k_side, k_side};
    d.display.zpos = static_cast<int>(i) + 3;
    auto h = scene.add_layer(std::move(d));
    ASSERT_TRUE(h.has_value()) << h.error().message();
    handles.push_back(*h);
  }
  ASSERT_TRUE(scene.commit().has_value());
  ASSERT_TRUE(scene.commit().has_value());

  for (std::size_t i = 0; i < handles.size(); ++i) {
    auto* layer = scene.get_layer(handles[i]);
    ASSERT_NE(layer, nullptr);
    layer->set_zpos(static_cast<int>(handles.size() - i) + 2);  // reverse
  }
  ASSERT_TRUE(scene.commit().has_value());

  auto img_r = snapshot(dev, active.crtc_id);
  drmModeSetCrtc(dev.fd(), active.crtc_id, 0, 0, 0, nullptr, 0, nullptr);
  ASSERT_TRUE(img_r.has_value()) << img_r.error().message();
  const auto cx = static_cast<std::uint32_t>(x) + (k_side / 2U);
  const auto cy = static_cast<std::uint32_t>(y) + (k_side / 2U);
  EXPECT_EQ(img_r->pixels()[(cy * img_r->width()) + cx], colors.front())
      << "the first layer, now topmost, must be visible";
}

// Removing layers until the rest fit the planes must take the composited ones
// off the canvas. Where planes have no zpos property, warm start kept them
// composited: the cached run was still in plane order and the kernel accepted
// it (#341).
TEST(LayerSceneCompositionVkms, CompositedLayersReturnToPlanesAfterRemoval) {
  const auto node = drm::test::find_vkms_node();
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

  LayerScene::Config cfg;
  cfg.crtc_id = active.crtc_id;
  cfg.connector_id = active.connector_id;
  cfg.mode = active.mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  constexpr std::uint32_t k_layers = 16;  // more than vkms has planes
  constexpr std::uint32_t k_side = 64;
  constexpr std::size_t k_overflow = 4;
  std::vector<drm::scene::LayerHandle> handles;
  for (std::uint32_t i = 0; i < k_layers; ++i) {
    auto src = DumbBufferSource::create(dev, k_side, k_side, DRM_FORMAT_ARGB8888);
    ASSERT_TRUE(src.has_value()) << src.error().message();
    fill_uniform_argb(**src, k_side, k_side, 0xFF000000U | ((i * 15U) << 16U) | 0x80U);
    LayerDesc d;
    d.source = std::move(*src);
    d.display.src_rect = drm::scene::Rect{0, 0, k_side, k_side};
    const auto off = static_cast<std::int32_t>(i * 8U);
    d.display.dst_rect = drm::scene::Rect{off, off, k_side, k_side};
    d.display.zpos = static_cast<int>(i) + 3;
    auto h = scene.add_layer(std::move(d));
    ASSERT_TRUE(h.has_value()) << h.error().message();
    handles.push_back(*h);
  }
  auto first = scene.commit();
  ASSERT_TRUE(first.has_value()) << first.error().message();
  ASSERT_GT(first->layers_composited, 0U) << "the stack should overflow into the canvas";
  const std::size_t planes_for_layers = first->layers_assigned;  // one more holds the canvas

  // Removing placed layers frees their planes. Only k_overflow layers still
  // need the canvas; warm start kept all of the old run there.
  auto remove_top = [&](std::size_t keep) {
    while (handles.size() > keep) {
      scene.remove_layer(handles.back());
      handles.pop_back();
    }
  };
  remove_top(planes_for_layers + k_overflow);
  for (int f = 0; f < 2; ++f) {
    auto r = scene.commit();
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(r->layers_composited, k_overflow) << "frame " << f;
    EXPECT_EQ(r->layers_assigned, planes_for_layers) << "frame " << f;
  }

  // Drop the top k_overflow: what is left fits one plane per layer.
  remove_top(planes_for_layers);
  for (int f = 0; f < 2; ++f) {
    auto r = scene.commit();
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(r->layers_composited, 0U) << "frame " << f;
    EXPECT_EQ(r->layers_assigned, planes_for_layers) << "frame " << f;
  }
  drmModeSetCrtc(dev.fd(), active.crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}

namespace {

// Planes holding a framebuffer on `crtc_id`.
std::vector<std::uint32_t> lit_planes(int fd, std::uint32_t crtc_id) {
  std::vector<std::uint32_t> out;
  auto* res = drmModeGetPlaneResources(fd);
  if (res == nullptr) {
    return out;
  }
  for (std::uint32_t i = 0; i < res->count_planes; ++i) {
    auto* p = drmModeGetPlane(fd, res->planes[i]);
    if (p != nullptr && p->fb_id != 0 && p->crtc_id == crtc_id) {
      out.push_back(p->plane_id);
    }
    drmModeFreePlane(p);
  }
  drmModeFreePlaneResources(res);
  return out;
}

}  // namespace

// A plane another client left lit (the fbdev console restores its framebuffer
// on last close) must go off on the first commit when the scene does not use
// it. Every TEST disabled it; the real commit left it scanning out (#342).
TEST(LayerSceneCompositionVkms, FirstCommitTurnsOffForeignPlanes) {
  const auto node = drm::test::find_vkms_node();
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
  if (lit_planes(dev.fd(), active.crtc_id).empty()) {
    GTEST_SKIP() << "no plane lit on the CRTC before the scene (no fbdev console)";
  }

  LayerScene::Config cfg;
  cfg.crtc_id = active.crtc_id;
  cfg.connector_id = active.connector_id;
  cfg.mode = active.mode;
  auto scene_r = LayerScene::create(dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << scene_r.error().message();
  auto& scene = **scene_r;

  // Topmost zpos: where planes stack by id, the layer lands on the last
  // plane, not on the one the console holds.
  constexpr std::uint32_t k_side = 64;
  auto src = DumbBufferSource::create(dev, k_side, k_side, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(src.has_value()) << src.error().message();
  fill_uniform_argb(**src, k_side, k_side, 0xFF00FF00U);
  LayerDesc d;
  d.source = std::move(*src);
  d.display.src_rect = drm::scene::Rect{0, 0, k_side, k_side};
  d.display.dst_rect = drm::scene::Rect{64, 64, k_side, k_side};
  d.display.zpos = 64;
  auto h = scene.add_layer(std::move(d));
  ASSERT_TRUE(h.has_value()) << h.error().message();

  auto report = scene.commit();
  ASSERT_TRUE(report.has_value()) << report.error().message();
  ASSERT_EQ(report->layers_assigned, 1U);
  const auto lit = lit_planes(dev.fd(), active.crtc_id);
  drmModeSetCrtc(dev.fd(), active.crtc_id, 0, 0, 0, nullptr, 0, nullptr);
  EXPECT_EQ(lit.size(), 1U) << "a plane the scene does not use is still lit";
}
