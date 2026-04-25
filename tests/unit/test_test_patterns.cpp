// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Stride-aware sanity checks for the deterministic test_patterns
// painters. Pixel-perfect golden-image tests would couple this file to
// rounding choices that should remain easy to retune; the assertions
// below exercise the entry points and check a handful of invariants
// (column-0 colour, tile boundaries, gradient endpoints) instead.

#include "test_patterns/patterns.hpp"

#include <drm-cxx/detail/span.hpp>

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace {

using drm::examples::test_patterns::PaintTarget;
using drm::examples::test_patterns::PatternKind;

// Allocate a buffer with a stride that deliberately exceeds width*4 so
// the painters' row-pointer arithmetic is exercised against padded
// scanlines. 16-byte pad chosen to stay aligned and small.
struct TestBuffer {
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t stride_bytes;
  std::vector<std::uint8_t> bytes;

  TestBuffer(std::uint32_t w, std::uint32_t h, std::uint32_t pad_bytes = 16)
      : width(w),
        height(h),
        stride_bytes((w * 4U) + pad_bytes),
        bytes(static_cast<std::size_t>(stride_bytes) * h, 0xAA) {}

  PaintTarget target() {
    PaintTarget t;
    t.pixels = drm::span<std::uint8_t>(bytes.data(), bytes.size());
    t.stride_bytes = stride_bytes;
    t.width = width;
    t.height = height;
    return t;
  }

  // XRGB8888 read at (x, y).
  [[nodiscard]] std::uint32_t at(std::uint32_t x, std::uint32_t y) const {
    const std::size_t off =
        (static_cast<std::size_t>(y) * stride_bytes) + (static_cast<std::size_t>(x) * 4U);
    return static_cast<std::uint32_t>(bytes[off]) |
           (static_cast<std::uint32_t>(bytes[off + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[off + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[off + 3U]) << 24U);
  }
};

}  // namespace

TEST(TestPatterns, PaintRejectsZeroDims) {
  TestBuffer buf(0, 0);
  // Should be a quiet no-op — buf was sentinel-filled with 0xAA; nothing
  // should have changed.
  drm::examples::test_patterns::paint(PatternKind::SmpteBars, buf.target());
  // bytes vector is empty when width/height are 0; no assertion possible
  // beyond "didn't crash".
  SUCCEED();
}

TEST(TestPatterns, PaintRejectsShortSpan) {
  TestBuffer buf(64, 64);
  PaintTarget t = buf.target();
  // Lie about the height so required = 128 * stride > pixels.size().
  t.height = 128;
  drm::examples::test_patterns::paint(PatternKind::PixelStripes, t);
  // Sentinel must still be present at (0,0) — the painter bailed
  // before touching the buffer.
  EXPECT_EQ(buf.bytes[0], 0xAAU);
}

TEST(TestPatterns, PixelStripesAlternate) {
  TestBuffer buf(16, 4);
  drm::examples::test_patterns::paint(PatternKind::PixelStripes, buf.target());
  // Even columns white, odd columns black; both with alpha byte == 0
  // (XRGB8888, alpha is don't-care).
  EXPECT_EQ(buf.at(0, 0) & 0x00FFFFFFU, 0x00FFFFFFU);
  EXPECT_EQ(buf.at(1, 0) & 0x00FFFFFFU, 0x00000000U);
  EXPECT_EQ(buf.at(2, 0) & 0x00FFFFFFU, 0x00FFFFFFU);
  EXPECT_EQ(buf.at(15, 3) & 0x00FFFFFFU, 0x00000000U);
}

TEST(TestPatterns, CheckerboardTilesDiffer) {
  // Two-tile-wide buffer (128 px) so we can sample inside both tiles.
  TestBuffer buf(128, 128);
  drm::examples::test_patterns::paint(PatternKind::Checkerboard, buf.target());
  // (0, 0) and (64, 0) lie in adjacent tiles; their fill colours must
  // differ. Don't assert the exact colours — kCheckerTile could be
  // retuned and we'd rather the test survive that.
  EXPECT_NE(buf.at(0, 0) & 0x00FFFFFFU, buf.at(64, 0) & 0x00FFFFFFU);
  // (0, 0) and (0, 64) are in vertically adjacent tiles — also differ.
  EXPECT_NE(buf.at(0, 0) & 0x00FFFFFFU, buf.at(0, 64) & 0x00FFFFFFU);
  // (0, 0) and (64, 64) are diagonally adjacent — the same colour.
  EXPECT_EQ(buf.at(0, 0) & 0x00FFFFFFU, buf.at(64, 64) & 0x00FFFFFFU);
}

TEST(TestPatterns, GrayBarsBoundaryFromBlack) {
  // 11 bars across 110 px → bar_w = 10. The first bar is code 0 (black);
  // the second bar starts at column 10 with code 26.
  TestBuffer buf(110, 4);
  drm::examples::test_patterns::paint(PatternKind::GrayBars, buf.target());
  EXPECT_EQ(buf.at(0, 0) & 0x00FFFFFFU, 0x00000000U);
  // Column 9 is the last pixel of the 0 % bar → still black.
  EXPECT_EQ(buf.at(9, 0) & 0x00FFFFFFU, 0x00000000U);
  // Column 10 is the first pixel of the 10 % bar → gray code 26.
  EXPECT_EQ(buf.at(10, 0) & 0x00FFFFFFU, 0x001A1A1AU);
  // Last column → 100 % white.
  EXPECT_EQ(buf.at(109, 0) & 0x00FFFFFFU, 0x00FFFFFFU);
}

TEST(TestPatterns, SmpteBarsTopBandEndpoints) {
  // 7 top-band bars across 70 px → bar_w = 10. Expectation: leftmost
  // column is 75 % gray (code 191); rightmost column is 75 % blue.
  TestBuffer buf(70, 30);
  drm::examples::test_patterns::paint(PatternKind::SmpteBars, buf.target());
  // (0, 0) is inside the gray bar.
  EXPECT_EQ(buf.at(0, 0) & 0x00FFFFFFU, 0x00BFBFBFU);
  // (69, 0) is in the rightmost bar (blue) of the top band. Top band
  // height = (30 * 2) / 3 = 20, so y = 0 is well inside.
  EXPECT_EQ(buf.at(69, 0) & 0x00FFFFFFU, 0x000000BFU);
}

TEST(TestPatterns, GrayGradientEndpoints) {
  TestBuffer buf(256, 4);
  drm::examples::test_patterns::paint(PatternKind::GrayGradient, buf.target());
  // First column → black; last column → white.
  EXPECT_EQ(buf.at(0, 0) & 0x00FFFFFFU, 0x00000000U);
  EXPECT_EQ(buf.at(255, 0) & 0x00FFFFFFU, 0x00FFFFFFU);
}

TEST(TestPatterns, NameOfReturnsNonEmptyForAllKinds) {
  for (std::uint8_t i = 0; i < drm::examples::test_patterns::k_pattern_count; ++i) {
    const auto* name = drm::examples::test_patterns::name_of(static_cast<PatternKind>(i));
    ASSERT_NE(name, nullptr);
    EXPECT_NE(name[0], '\0');
  }
}
