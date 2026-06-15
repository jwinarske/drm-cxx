// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Pixel-correctness check for the GPU compositor (GlCompositor): GPU-composite a
// known opaque-red square at the canvas top-left, read the result back via
// gbm_bo map, and verify the red lands top-left (Y-orientation), in the red
// channel (BGRA swizzle), over a transparent-black clear (blend). This is what
// the kernel's TEST_ONLY commit can't check.
//
// The pixel invariants checked here (orientation, channel order, blend) are
// renderer-independent — llvmpipe runs the same GLES2 shader and geometry as a
// hardware GPU — so the probe sets allow_software_renderer and validates the GL
// pixel pipeline wherever EGL plus any GL renderer is available, including CI's
// llvmpipe-on-vkms. It renders offscreen and reads back, so it needs neither
// DRM master nor a commit and runs with a desktop up. Skips only when EGL is
// not built or no GL context can be created at all. (Hardware GPU acceleration
// itself — kmsro routing, real scanout — is covered by the mechanics demos.)

#include <gtest/gtest.h>

#if DRM_CXX_HAS_EGL

#include "scene/composite_canvas.hpp"
#include "scene/composition_target.hpp"
#include "scene/gl_compositor.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// First card that is KMS-capable (has a CRTC — skips render-only nodes like
// PowerVR pvrsrvkm) and on which a GlCompositor can be created.
std::optional<drm::Device> open_gl_kms_device() {
  for (int i = 0; i < 8; ++i) {
    auto dev = drm::Device::open("/dev/dri/card" + std::to_string(i));
    if (!dev) {
      continue;
    }
    drmModeRes* res = drmModeGetResources(dev->fd());
    if (res == nullptr) {
      continue;
    }
    const bool kms = res->count_crtcs > 0;
    drmModeFreeResources(res);
    if (!kms) {
      continue;
    }
    drm::scene::CompositeCanvasConfig cfg;
    cfg.canvas_width = 64;
    cfg.canvas_height = 64;
    cfg.allow_software_renderer = true;  // validate GL pixel math even on llvmpipe (CI/host)
    auto probe = drm::scene::GlCompositor::create(*dev, cfg);
    if (probe) {
      return {std::move(*dev)};
    }
  }
  return std::nullopt;
}

}  // namespace

TEST(GlCompositorReadback, RedSquareLandsTopLeftInRedChannel) {
  auto dev = open_gl_kms_device();
  if (!dev) {
    GTEST_SKIP() << "no GL-capable KMS card (vkms/CI or GPU-less) — GPU path not exercisable";
  }

  constexpr std::uint32_t k_canvas = 256;
  constexpr std::uint32_t k_square = 64;
  drm::scene::CompositeCanvasConfig cfg;
  cfg.canvas_width = k_canvas;
  cfg.canvas_height = k_canvas;
  cfg.output_fourcc = DRM_FORMAT_ARGB8888;
  cfg.allow_software_renderer = true;
  auto comp = drm::scene::GlCompositor::create(*dev, cfg);
  ASSERT_TRUE(comp.has_value()) << comp.error().message();

  // Opaque-red XRGB8888 source (memory order B,G,R,X = 00,00,FF,00).
  std::vector<std::uint8_t> red(static_cast<std::size_t>(k_square) * k_square * 4U, 0U);
  for (std::size_t i = 0; i < red.size(); i += 4U) {
    red[i + 2U] = 0xFFU;  // R
  }
  drm::scene::CompositeSrc src;
  src.pixels = drm::span<const std::uint8_t>(red.data(), red.size());
  src.src_stride_bytes = k_square * 4U;
  src.src_width = k_square;
  src.src_height = k_square;
  src.drm_fourcc = DRM_FORMAT_XRGB8888;

  (*comp)->begin_frame();
  (*comp)->clear();
  (*comp)->blend(src, drm::scene::CompositeRect{0, 0, k_square, k_square},
                 drm::scene::CompositeRect{0, 0, k_square, k_square});
  ASSERT_TRUE((*comp)->flush().has_value());

  std::vector<std::uint8_t> px;
  ASSERT_TRUE((*comp)->read_back(px).has_value());
  ASSERT_EQ(px.size(), static_cast<std::size_t>(k_canvas) * k_canvas * 4U);

  auto at = [&](std::uint32_t x, std::uint32_t y) {
    return px.data() + ((static_cast<std::size_t>(y) * k_canvas + x) * 4U);
  };
  // Inside the square (top-left): red set, blue clear — proves Y-orientation
  // (red at TOP not bottom) and channel order (R not B).
  const std::uint8_t* in = at(10, 10);
  EXPECT_EQ(in[2], 0xFFU) << "red channel not set inside the square";
  EXPECT_EQ(in[0], 0x00U) << "blue channel set — BGRA swizzle wrong";
  // Outside the square: transparent black (clear worked, no bleed).
  const std::uint8_t* out = at(200, 200);
  EXPECT_EQ(out[0], 0x00U);
  EXPECT_EQ(out[1], 0x00U);
  EXPECT_EQ(out[2], 0x00U);
}

// Guard invariant: with the default config (allow_software_renderer == false),
// GlCompositor::create must reject a software renderer so LayerScene's Auto mode
// falls back to the CPU canvas instead of compositing through llvmpipe. Where
// only a software GL stack exists (CI llvmpipe-on-vkms, or a host whose GPU is
// busy) the default create fails with not_supported while an allow_software
// create on the same device succeeds. On real GPU hardware the default create
// succeeds and the guard isn't exercised, so the case skips.
TEST(GlCompositorReadback, SoftwareRendererRejectedByDefault) {
  auto dev = open_gl_kms_device();
  if (!dev) {
    GTEST_SKIP() << "no GL-capable KMS card — guard not exercisable";
  }

  drm::scene::CompositeCanvasConfig strict;
  strict.canvas_width = 64;
  strict.canvas_height = 64;  // allow_software_renderer defaults to false
  auto def = drm::scene::GlCompositor::create(*dev, strict);
  if (def) {
    GTEST_SKIP() << "hardware GL renderer present — software guard not exercised here";
  }
  EXPECT_EQ(def.error(), std::make_error_code(std::errc::not_supported));

  drm::scene::CompositeCanvasConfig relaxed = strict;
  relaxed.allow_software_renderer = true;
  auto sw = drm::scene::GlCompositor::create(*dev, relaxed);
  EXPECT_TRUE(sw.has_value())
      << "allow_software create should succeed where the strict create was rejected";
}

#else

TEST(GlCompositorReadback, SkippedNoEgl) {
  GTEST_SKIP() << "DRM_CXX_HAS_EGL not built";
}

#endif  // DRM_CXX_HAS_EGL
