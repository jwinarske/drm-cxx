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
#include <gbm.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
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

// Import a dma-buf directly and verify the imported texture samples in the
// correct channel order (no CPU pixels, so only the EGLImage path can produce
// output). This is the escape from the map()-uncompositable trap for camera
// layers. Skips when the stack can't import dma-bufs.
TEST(GlCompositorReadback, DmaBufImportSamplesCorrectChannels) {
  auto dev = open_gl_kms_device();
  if (!dev) {
    GTEST_SKIP() << "no GL-capable KMS card — GPU path not exercisable";
  }
  constexpr std::uint32_t k_canvas = 256;
  constexpr std::uint32_t k_sq = 64;
  drm::scene::CompositeCanvasConfig cfg;
  cfg.canvas_width = k_canvas;
  cfg.canvas_height = k_canvas;
  cfg.output_fourcc = DRM_FORMAT_ARGB8888;
  cfg.allow_software_renderer = true;
  auto comp = drm::scene::GlCompositor::create(*dev, cfg);
  ASSERT_TRUE(comp.has_value()) << comp.error().message();
  if (!(*comp)->dmabuf_import_supported()) {
    GTEST_SKIP() << "EGL_EXT_image_dma_buf_import not available on this stack";
  }

  // Allocate a linear ARGB gbm bo and fill it opaque red (memory B,G,R,A).
  gbm_device* gbm = gbm_create_device(dev->fd());
  ASSERT_NE(gbm, nullptr);
  gbm_bo* bo =
      gbm_bo_create(gbm, k_sq, k_sq, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
  if (bo == nullptr) {
    gbm_device_destroy(gbm);
    GTEST_SKIP() << "gbm_bo_create(ARGB, LINEAR) unsupported on this stack";
  }
  void* map_data = nullptr;
  std::uint32_t map_stride = 0;
  void* ptr = gbm_bo_map(bo, 0, 0, k_sq, k_sq, GBM_BO_TRANSFER_WRITE, &map_stride, &map_data);
  ASSERT_NE(ptr, nullptr);
  auto* pix = static_cast<std::uint8_t*>(ptr);
  for (std::uint32_t y = 0; y < k_sq; ++y) {
    for (std::uint32_t x = 0; x < k_sq; ++x) {
      std::uint8_t* p = pix + (static_cast<std::size_t>(y) * map_stride) + (x * 4U);
      p[0] = 0x00U;  // B
      p[1] = 0x00U;  // G
      p[2] = 0xFFU;  // R
      p[3] = 0xFFU;  // A
    }
  }
  gbm_bo_unmap(bo, map_data);

  const int fd = gbm_bo_get_fd(bo);
  ASSERT_GE(fd, 0);
  drm::scene::CompositeSrc src;
  src.src_width = k_sq;
  src.src_height = k_sq;
  src.drm_fourcc = DRM_FORMAT_ARGB8888;
  src.dma_n_planes = 1;
  src.dma_fds[0] = fd;
  src.dma_offsets[0] = static_cast<std::uint32_t>(gbm_bo_get_offset(bo, 0));
  src.dma_pitches[0] = gbm_bo_get_stride(bo);
  src.dma_modifier = gbm_bo_get_modifier(bo);
  // pixels intentionally left empty: only the dma-buf import path can draw here.

  (*comp)->begin_frame();
  (*comp)->clear();
  (*comp)->blend(src, drm::scene::CompositeRect{0, 0, k_sq, k_sq},
                 drm::scene::CompositeRect{0, 0, k_sq, k_sq});
  ASSERT_TRUE((*comp)->flush().has_value());

  std::vector<std::uint8_t> readpx;
  ASSERT_TRUE((*comp)->read_back(readpx).has_value());
  ASSERT_EQ(readpx.size(), static_cast<std::size_t>(k_canvas) * k_canvas * 4U);
  const std::uint8_t* in = readpx.data() + ((static_cast<std::size_t>(10) * k_canvas + 10) * 4U);
  EXPECT_EQ(in[2], 0xFFU) << "imported dma-buf: red channel not sampled";
  EXPECT_EQ(in[0], 0x00U) << "imported dma-buf: blue set — wrong channel order (swizzle applied?)";

  ::close(fd);
  gbm_bo_destroy(bo);
  gbm_device_destroy(gbm);
}

// NV12 external-sampler import: build a two-plane linear NV12 dma-buf filled
// full-luma / neutral-chroma (Y=0xFF, U=V=0x80 => white), composite it over a
// transparent-black clear via the GL_TEXTURE_EXTERNAL_OES path, and verify the
// overlay region reads back bright on all channels while the cleared background
// stays dark. Neutral chroma keeps the check independent of the driver's
// YUV->RGB matrix / range. Skips when the stack lacks the external sampler.
TEST(GlCompositorReadback, Nv12DmaBufImportSamplesAsWhite) {
  auto dev = open_gl_kms_device();
  if (!dev) {
    GTEST_SKIP() << "no GL-capable KMS card — GPU path not exercisable";
  }
  constexpr std::uint32_t k_canvas = 256;
  constexpr std::uint32_t k_sq = 64;
  drm::scene::CompositeCanvasConfig cfg;
  cfg.canvas_width = k_canvas;
  cfg.canvas_height = k_canvas;
  cfg.output_fourcc = DRM_FORMAT_ARGB8888;
  cfg.allow_software_renderer = true;
  auto comp = drm::scene::GlCompositor::create(*dev, cfg);
  ASSERT_TRUE(comp.has_value()) << comp.error().message();
  if (!(*comp)->supports_dma_buf_import(DRM_FORMAT_NV12)) {
    GTEST_SKIP() << "no external-sampler NV12 import on this stack";
  }

  // Mesa gbm rejects a native NV12 allocation on the drivers seen here (amdgpu,
  // vc4/v3d), so back the NV12 with a single linear R8 buffer sized
  // width x (height * 3/2): the canonical single-handle NV12 layout, with Y in
  // the first `height` rows and interleaved UV in the next `height/2`. Both
  // planes share the one fd at different offsets.
  gbm_device* gbm = gbm_create_device(dev->fd());
  ASSERT_NE(gbm, nullptr);
  gbm_bo* bo = gbm_bo_create(gbm, k_sq, k_sq + (k_sq / 2U), GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
  if (bo == nullptr) {
    gbm_device_destroy(gbm);
    GTEST_SKIP() << "gbm_bo_create(R8, LINEAR) unsupported on this stack";
  }
  void* map_data = nullptr;
  std::uint32_t map_stride = 0;
  void* ptr =
      gbm_bo_map(bo, 0, 0, k_sq, k_sq + (k_sq / 2U), GBM_BO_TRANSFER_WRITE, &map_stride, &map_data);
  if ((ptr == nullptr) || (map_stride != gbm_bo_get_stride(bo))) {
    if (ptr != nullptr) {
      gbm_bo_unmap(bo, map_data);
    }
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    GTEST_SKIP() << "gbm_bo_map(R8) unusable on this stack";
  }
  const std::uint32_t stride = map_stride;
  auto* base = static_cast<std::uint8_t*>(ptr);
  for (std::uint32_t y = 0; y < k_sq; ++y) {  // Y = full luma
    std::uint8_t* row = base + (static_cast<std::size_t>(y) * stride);
    for (std::uint32_t x = 0; x < k_sq; ++x) {
      row[x] = 0xFFU;
    }
  }
  for (std::uint32_t y = 0; y < k_sq / 2U; ++y) {  // interleaved UV = neutral
    std::uint8_t* row = base + (static_cast<std::size_t>(k_sq + y) * stride);
    for (std::uint32_t x = 0; x < k_sq; ++x) {
      row[x] = 0x80U;
    }
  }
  gbm_bo_unmap(bo, map_data);

  const int fd = gbm_bo_get_fd(bo);
  ASSERT_GE(fd, 0);
  drm::scene::CompositeSrc src;
  src.src_width = k_sq;
  src.src_height = k_sq;
  src.drm_fourcc = DRM_FORMAT_NV12;
  src.dma_n_planes = 2;
  src.dma_fds[0] = fd;
  src.dma_fds[1] = fd;  // same buffer, UV at offset stride*height
  src.dma_offsets[0] = 0;
  src.dma_offsets[1] = stride * k_sq;
  src.dma_pitches[0] = stride;
  src.dma_pitches[1] = stride;
  src.dma_modifier = gbm_bo_get_modifier(bo);
  // pixels intentionally empty: only the dma-buf import path can draw here.

  (*comp)->begin_frame();
  (*comp)->clear();
  (*comp)->blend(src, drm::scene::CompositeRect{0, 0, k_sq, k_sq},
                 drm::scene::CompositeRect{0, 0, k_sq, k_sq});
  ASSERT_TRUE((*comp)->flush().has_value());

  std::vector<std::uint8_t> readpx;
  ASSERT_TRUE((*comp)->read_back(readpx).has_value());
  ASSERT_EQ(readpx.size(), static_cast<std::size_t>(k_canvas) * k_canvas * 4U);
  const std::uint8_t* inside =
      readpx.data() + ((static_cast<std::size_t>(10) * k_canvas + 10) * 4U);
  EXPECT_GT(inside[0], 0x80U) << "NV12 overlay: blue channel dark — import/convert failed";
  EXPECT_GT(inside[1], 0x80U) << "NV12 overlay: green channel dark — import/convert failed";
  EXPECT_GT(inside[2], 0x80U) << "NV12 overlay: red channel dark — import/convert failed";
  const std::uint8_t* outside =
      readpx.data() + ((static_cast<std::size_t>(200) * k_canvas + 200) * 4U);
  EXPECT_LT(outside[2], 0x40U) << "background not cleared — overlay bled or clear failed";

  ::close(fd);
  gbm_bo_destroy(bo);
  gbm_device_destroy(gbm);
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
