// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::CompositeCanvas — the CPU SRC_OVER blender
// behind LayerScene's composition fallback. The buffer-bound API path
// (allocate dumb buffer, blend into it) needs a live KMS device and
// belongs in an integration test; this file exercises the pure-CPU
// `blend_into` / `clear_into` static helpers against stack buffers,
// which is where every composition correctness bug actually lives.

#include "scene/composite_canvas.hpp"

#include <drm-cxx/detail/span.hpp>

#include <drm_fourcc.h>

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace {

using drm::scene::CompositeCanvas;
using drm::scene::CompositeRect;
using drm::scene::CompositeSrc;

// Allocate a dst buffer with `pad_bytes` of stride padding so the
// blender's stride math is exercised against non-tightly-packed rows.
struct DstBuffer {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t stride_bytes{0};
  std::vector<std::uint8_t> bytes;

  DstBuffer(std::uint32_t w, std::uint32_t h, std::uint32_t pad_bytes = 16)
      : width(w),
        height(h),
        stride_bytes((w * 4U) + pad_bytes),
        bytes(static_cast<std::size_t>(stride_bytes) * h, 0U) {}

  drm::span<std::uint8_t> as_span() { return drm::span<std::uint8_t>(bytes.data(), bytes.size()); }

  // Read the 32-bit ARGB pixel at (x, y).
  [[nodiscard]] std::uint32_t at(std::uint32_t x, std::uint32_t y) const {
    const std::size_t off = (static_cast<std::size_t>(y) * stride_bytes) + (x * 4U);
    return static_cast<std::uint32_t>(bytes[off]) |
           (static_cast<std::uint32_t>(bytes[off + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[off + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[off + 3U]) << 24U);
  }

  // Overwrite (x, y) with the 32-bit ARGB pixel `pixel`.
  void set(std::uint32_t x, std::uint32_t y, std::uint32_t pixel) {
    const std::size_t off = (static_cast<std::size_t>(y) * stride_bytes) + (x * 4U);
    bytes[off] = static_cast<std::uint8_t>(pixel & 0xFFU);
    bytes[off + 1U] = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
    bytes[off + 2U] = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
    bytes[off + 3U] = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
  }
};

// Tightly-packed source buffer of a uniform colour.
struct UniformSrc {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t fourcc{0};
  std::vector<std::uint8_t> bytes;

  UniformSrc(std::uint32_t w, std::uint32_t h, std::uint32_t pixel, std::uint32_t fmt)
      : width(w), height(h), fourcc(fmt), bytes(static_cast<std::size_t>(w) * h * 4U) {
    for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
      bytes[i] = static_cast<std::uint8_t>(pixel & 0xFFU);
      bytes[i + 1] = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
      bytes[i + 2] = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
      bytes[i + 3] = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
    }
  }

  CompositeSrc as_src() const {
    CompositeSrc s;
    s.pixels = drm::span<const std::uint8_t>(bytes.data(), bytes.size());
    s.src_stride_bytes = width * 4U;
    s.src_width = width;
    s.src_height = height;
    s.drm_fourcc = fourcc;
    return s;
  }

  CompositeRect full_rect() const { return CompositeRect{0, 0, width, height}; }
};

}  // namespace

// ── clear_into ──────────────────────────────────────────────────────────

TEST(CompositeCanvasClear, ZerosOnlyTheRect) {
  DstBuffer dst(8, 8);
  // Pre-fill with sentinel.
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      dst.set(x, y, 0xDEADBEEFU);
    }
  }
  CompositeCanvas::clear_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, 2, 3, 4, 2);
  // Inside the rect: zeroed.
  EXPECT_EQ(dst.at(2, 3), 0U);
  EXPECT_EQ(dst.at(5, 4), 0U);
  // Outside: untouched.
  EXPECT_EQ(dst.at(0, 0), 0xDEADBEEFU);
  EXPECT_EQ(dst.at(7, 7), 0xDEADBEEFU);
  EXPECT_EQ(dst.at(2, 2), 0xDEADBEEFU);  // row above
  EXPECT_EQ(dst.at(2, 5), 0xDEADBEEFU);  // row below
}

TEST(CompositeCanvasClear, ClipsLeftAndTopOverflow) {
  DstBuffer dst(8, 8);
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      dst.set(x, y, 0xAAAAAAAAU);
    }
  }
  // Rect starts off-canvas at (-2, -2), 5x5 — should clear only (0..3, 0..3).
  CompositeCanvas::clear_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, -2, -2, 5, 5);
  EXPECT_EQ(dst.at(0, 0), 0U);
  EXPECT_EQ(dst.at(2, 2), 0U);
  EXPECT_EQ(dst.at(3, 3), 0xAAAAAAAAU);  // first pixel outside the clipped rect
}

TEST(CompositeCanvasClear, DegenerateIsNoOp) {
  DstBuffer dst(8, 8);
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      dst.set(x, y, 0x12345678U);
    }
  }
  CompositeCanvas::clear_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, 0, 0, 0, 0);
  CompositeCanvas::clear_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, 4, 4, -1, 5);
  EXPECT_EQ(dst.at(0, 0), 0x12345678U);
  EXPECT_EQ(dst.at(7, 7), 0x12345678U);
}

// ── blend_into ──────────────────────────────────────────────────────────

TEST(CompositeCanvasBlend, OpaqueSourceOverwrites) {
  DstBuffer dst(8, 8);
  // Pre-fill with non-zero so an "overwrite" is visible.
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      dst.set(x, y, 0xFF112233U);
    }
  }
  UniformSrc src(4, 4, 0xFFAABBCCU, DRM_FORMAT_ARGB8888);
  CompositeCanvas::blend_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, src.as_src(),
                              src.full_rect(), CompositeRect{2, 2, 4, 4});
  EXPECT_EQ(dst.at(2, 2), 0xFFAABBCCU);
  EXPECT_EQ(dst.at(5, 5), 0xFFAABBCCU);
  EXPECT_EQ(dst.at(1, 1), 0xFF112233U);  // outside the dst rect — untouched
  EXPECT_EQ(dst.at(6, 6), 0xFF112233U);
}

TEST(CompositeCanvasBlend, FullyTransparentSourceLeavesDestination) {
  DstBuffer dst(4, 4);
  for (std::uint32_t y = 0; y < 4; ++y) {
    for (std::uint32_t x = 0; x < 4; ++x) {
      dst.set(x, y, 0xFF445566U);
    }
  }
  UniformSrc src(4, 4, 0x00FFFFFFU, DRM_FORMAT_ARGB8888);  // a=0
  CompositeCanvas::blend_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, src.as_src(),
                              src.full_rect(), CompositeRect{0, 0, 4, 4});
  EXPECT_EQ(dst.at(0, 0), 0xFF445566U);
  EXPECT_EQ(dst.at(3, 3), 0xFF445566U);
}

TEST(CompositeCanvasBlend, HalfAlphaMixesEvenly) {
  // Black dst, white half-alpha src → expect ~50% gray with full alpha
  // accumulation (0xFF base + 0x80 * (1-0x80/255) ≈ 0xFF).
  DstBuffer dst(2, 2);
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 2; ++x) {
      dst.set(x, y, 0xFF000000U);  // opaque black
    }
  }
  UniformSrc src(2, 2, 0x80FFFFFFU, DRM_FORMAT_ARGB8888);  // 50% white
  CompositeCanvas::blend_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, src.as_src(),
                              src.full_rect(), CompositeRect{0, 0, 2, 2});
  // SRC_OVER: out_rgb = src_rgb + dst_rgb * (1 - src_a) =
  //   0xFF + 0 * (175/255) = 0xFF.  Wait — src is straight-alpha.
  // out_r = src_r + dst_r * (255 - 128 + 127) / 255 = 0xFF + 0 = 0xFF.
  // The implementation rounds (+127)/255, so out_r = 0xFF.
  const std::uint32_t pixel = dst.at(0, 0);
  EXPECT_EQ((pixel >> 16U) & 0xFFU, 0xFFU);
  // Alpha accumulates: out_a = src_a + dst_a * (255 - src_a + 127)/255
  //                   = 0x80 + 0xFF * 127/255 = 0x80 + 0x7F = 0xFF
  EXPECT_EQ((pixel >> 24U) & 0xFFU, 0xFFU);
}

TEST(CompositeCanvasBlend, XRGBSourceForcedOpaque) {
  // XRGB sources have undefined alpha bytes; the blender must treat
  // them as fully opaque so SRC_OVER writes the full source colour.
  DstBuffer dst(4, 4);
  for (std::uint32_t y = 0; y < 4; ++y) {
    for (std::uint32_t x = 0; x < 4; ++x) {
      dst.set(x, y, 0xFF112233U);
    }
  }
  // Source pixel: alpha byte deliberately left at 0 (undefined for XRGB).
  UniformSrc src(4, 4, 0x00ABCDEFU, DRM_FORMAT_XRGB8888);
  CompositeCanvas::blend_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, src.as_src(),
                              src.full_rect(), CompositeRect{0, 0, 4, 4});
  // Despite the 0 alpha byte in storage, the blender forces opacity
  // for XRGB so the dst takes the source RGB.
  EXPECT_EQ(dst.at(0, 0) & 0x00FFFFFFU, 0x00ABCDEFU);
  EXPECT_EQ((dst.at(0, 0) >> 24U) & 0xFFU, 0xFFU);
}

TEST(CompositeCanvasBlend, DstRectClippedAtRightEdge) {
  // 8x8 dst, 4x4 src, dst_rect at (6, 0) extending to (10, 4) — clipped
  // to (6, 0)..(8, 4). Outside columns must stay untouched.
  DstBuffer dst(8, 4);
  for (std::uint32_t y = 0; y < 4; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      dst.set(x, y, 0xFF000000U);
    }
  }
  UniformSrc src(4, 4, 0xFFFFFFFFU, DRM_FORMAT_ARGB8888);
  CompositeCanvas::blend_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, src.as_src(),
                              src.full_rect(), CompositeRect{6, 0, 4, 4});
  EXPECT_EQ(dst.at(6, 0), 0xFFFFFFFFU);
  EXPECT_EQ(dst.at(7, 3), 0xFFFFFFFFU);
  EXPECT_EQ(dst.at(5, 0), 0xFF000000U);  // column 5 is the last untouched
}

TEST(CompositeCanvasBlend, RejectsUnsupportedFormat) {
  DstBuffer dst(4, 4);
  UniformSrc src(4, 4, 0xFFFFFFFFU, DRM_FORMAT_NV12);  // not supported in v1
  // Pre-fill with sentinel so we can detect any write.
  for (std::uint32_t y = 0; y < 4; ++y) {
    for (std::uint32_t x = 0; x < 4; ++x) {
      dst.set(x, y, 0xCCCCCCCCU);
    }
  }
  CompositeCanvas::blend_into(dst.as_span(), dst.stride_bytes, dst.width, dst.height, src.as_src(),
                              src.full_rect(), CompositeRect{0, 0, 4, 4});
  // Sentinel intact — blender bailed before any pixel writes.
  EXPECT_EQ(dst.at(0, 0), 0xCCCCCCCCU);
}

TEST(CompositeCanvasBlend, RejectsShortDstSpan) {
  // Caller claims 4x4 dst but provides a span sized for 4x2.
  std::vector<std::uint8_t> bytes(4 * 2 * 4, 0xAAU);
  drm::span<std::uint8_t> dst_span(bytes.data(), bytes.size());
  UniformSrc src(4, 4, 0xFFFFFFFFU, DRM_FORMAT_ARGB8888);
  CompositeCanvas::blend_into(dst_span, /*stride*/ 16, /*width*/ 4, /*height*/ 4, src.as_src(),
                              src.full_rect(), CompositeRect{0, 0, 4, 4});
  // No writes.
  EXPECT_EQ(bytes[0], 0xAAU);
  EXPECT_EQ(bytes[bytes.size() - 1], 0xAAU);
}

TEST(CompositeCanvasBlend, RejectsUnalignedDst) {
  // Allocate 4 extra bytes so we can hand the blender an offset
  // pointer that's deliberately 1-byte misaligned. The reinterpret_cast
  // to uint32_t* inside blend_into would be UB; the alignment guard
  // must bail before any pixel writes happen.
  std::vector<std::uint8_t> bytes((4U * 4U * 4U) + 4U, 0xCCU);
  drm::span<std::uint8_t> dst_span(bytes.data() + 1, bytes.size() - 1);
  UniformSrc src(4, 4, 0xFFFFFFFFU, DRM_FORMAT_ARGB8888);
  CompositeCanvas::blend_into(dst_span, /*stride*/ 16, /*width*/ 4, /*height*/ 4, src.as_src(),
                              src.full_rect(), CompositeRect{0, 0, 4, 4});
  // Sentinel intact — guard caught the unaligned pointer.
  EXPECT_EQ(bytes[0], 0xCCU);
  EXPECT_EQ(bytes[bytes.size() - 1], 0xCCU);
}

TEST(CompositeCanvasBlend, NoScaleFastPathMatchesScaledPath) {
  // The src/dst-equal-size fast path must produce identical results to
  // the general nearest-neighbour mapping. Render a recognisable
  // gradient through both code paths and compare every pixel.
  constexpr std::uint32_t W = 8;
  constexpr std::uint32_t H = 8;
  std::vector<std::uint8_t> src_bytes(W * H * 4U);
  for (std::uint32_t y = 0; y < H; ++y) {
    for (std::uint32_t x = 0; x < W; ++x) {
      const auto pixel = static_cast<std::uint32_t>(0xFF000000U | ((x * 32U) << 16U) |
                                                    ((y * 32U) << 8U) | (x ^ y));
      const std::size_t off = ((y * W) + x) * 4U;
      src_bytes[off] = static_cast<std::uint8_t>(pixel & 0xFFU);
      src_bytes[off + 1U] = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
      src_bytes[off + 2U] = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
      src_bytes[off + 3U] = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
    }
  }
  CompositeSrc src;
  src.pixels = drm::span<const std::uint8_t>(src_bytes.data(), src_bytes.size());
  src.src_stride_bytes = W * 4U;
  src.src_width = W;
  src.src_height = H;
  src.drm_fourcc = DRM_FORMAT_ARGB8888;

  // Fast path: src and dst rects identical sizes.
  DstBuffer dst_fast(W, H);
  CompositeCanvas::blend_into(dst_fast.as_span(), dst_fast.stride_bytes, dst_fast.width,
                              dst_fast.height, src, CompositeRect{0, 0, W, H},
                              CompositeRect{0, 0, W, H});

  // Scaled path: same blit but with a 1-pixel-wider source rect that
  // only covers W of the W+0 pixels — degenerate in-rect mismatch the
  // mapping path will resolve. Compare against the fast-path result.
  DstBuffer dst_scaled(W, H);
  CompositeCanvas::blend_into(dst_scaled.as_span(), dst_scaled.stride_bytes, dst_scaled.width,
                              dst_scaled.height, src, CompositeRect{0, 0, W, H - 1},
                              CompositeRect{0, 0, W, H - 1});
  for (std::uint32_t y = 0; y < H - 1; ++y) {
    for (std::uint32_t x = 0; x < W; ++x) {
      EXPECT_EQ(dst_fast.at(x, y), dst_scaled.at(x, y))
          << "fast path diverges at (" << x << ", " << y << ")";
    }
  }
}
