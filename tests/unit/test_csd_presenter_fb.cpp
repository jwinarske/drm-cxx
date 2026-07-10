// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include <drm-cxx/csd/presenter_fb.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_fourcc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace {

using drm::csd::compose_into_framebuffer;
using drm::csd::fb_fourcc_for;
using drm::csd::FbBlitItem;

// One opaque ARGB8888 pixel, memory order B,G,R,A.
constexpr std::array<std::uint8_t, 4> px(std::uint8_t b, std::uint8_t g, std::uint8_t r,
                                         std::uint8_t a = 0xFF) {
  return {b, g, r, a};
}

}  // namespace

// ── fb_fourcc_for ──────────────────────────────────────────────────────

TEST(FbFourccFor, ThirtyTwoBppRedAboveBlue) {
  // red@16, blue@0 => memory B,G,R,(A).
  EXPECT_EQ(fb_fourcc_for(32, 16, 0, 0), DRM_FORMAT_XRGB8888);
  EXPECT_EQ(fb_fourcc_for(32, 16, 0, 8), DRM_FORMAT_ARGB8888);
}

TEST(FbFourccFor, ThirtyTwoBppRedBelowBlue) {
  // red@0, blue@16 => memory R,G,B,(A).
  EXPECT_EQ(fb_fourcc_for(32, 0, 16, 0), DRM_FORMAT_XBGR8888);
  EXPECT_EQ(fb_fourcc_for(32, 0, 16, 8), DRM_FORMAT_ABGR8888);
}

TEST(FbFourccFor, SixteenBpp) {
  EXPECT_EQ(fb_fourcc_for(16, 11, 0, 0), DRM_FORMAT_RGB565);
  EXPECT_EQ(fb_fourcc_for(16, 0, 11, 0), DRM_FORMAT_BGR565);
}

TEST(FbFourccFor, UnsupportedDepthIsNullopt) {
  EXPECT_FALSE(fb_fourcc_for(8, 0, 0, 0).has_value());
  EXPECT_FALSE(fb_fourcc_for(24, 16, 0, 0).has_value());
}

// ── compose_into_framebuffer ───────────────────────────────────────────

// A single opaque red decoration blitted at (1,0) into a 3x2 XRGB8888 fb
// lands exactly at that pixel and nowhere else; the rest stays cleared.
TEST(ComposeIntoFramebuffer, BlitsOneOpaquePixelAtOffset) {
  constexpr std::uint32_t w = 3;
  constexpr std::uint32_t h = 2;
  std::vector<std::uint8_t> fb(static_cast<std::size_t>(w) * h * 4U, 0xEE);  // non-zero pre-fill
  std::vector<std::uint8_t> shadow(static_cast<std::size_t>(w) * h * 4U, 0U);

  const auto red = px(0x00, 0x00, 0xFF);  // B,G,R,A
  const FbBlitItem item{
      drm::span<const std::uint8_t>(red.data(), red.size()), 4U, 1U, 1U, 1, 0, DRM_FORMAT_ARGB8888};

  compose_into_framebuffer(
      drm::span<std::uint8_t>(fb.data(), fb.size()), w * 4U, w, h, DRM_FORMAT_XRGB8888,
      drm::span<std::uint8_t>(shadow.data(), shadow.size()), drm::span<const FbBlitItem>(&item, 1));

  // Pixel (1,0) is red; (0,0) and (2,0) and row 1 are cleared to zero.
  const auto at = [&](std::uint32_t x, std::uint32_t y) {
    return &fb[(static_cast<std::size_t>(y) * w + x) * 4U];
  };
  EXPECT_EQ(at(1, 0)[2], 0xFF);  // R
  EXPECT_EQ(at(1, 0)[0], 0x00);  // B
  EXPECT_EQ(at(0, 0)[0], 0x00);  // cleared, not the 0xEE pre-fill
  EXPECT_EQ(at(2, 0)[2], 0x00);
  EXPECT_EQ(at(0, 1)[0], 0x00);
}

// XBGR8888 destination swaps R and B relative to the ARGB shadow.
TEST(ComposeIntoFramebuffer, ConvertsToXbgrDestination) {
  constexpr std::uint32_t w = 1;
  constexpr std::uint32_t h = 1;
  std::vector<std::uint8_t> fb(4U, 0U);
  std::vector<std::uint8_t> shadow(4U, 0U);

  const auto red = px(0x00, 0x00, 0xFF);  // ARGB source: R=0xFF
  const FbBlitItem item{
      drm::span<const std::uint8_t>(red.data(), red.size()), 4U, 1U, 1U, 0, 0, DRM_FORMAT_ARGB8888};
  compose_into_framebuffer(
      drm::span<std::uint8_t>(fb.data(), fb.size()), 4U, w, h, DRM_FORMAT_XBGR8888,
      drm::span<std::uint8_t>(shadow.data(), shadow.size()), drm::span<const FbBlitItem>(&item, 1));
  // XBGR8888 memory order R,G,B,X: red ends up in byte 0.
  EXPECT_EQ(fb[0], 0xFF);
  EXPECT_EQ(fb[2], 0x00);
}

// An undersized shadow is a no-op (defensive: never writes past the fb).
TEST(ComposeIntoFramebuffer, UndersizedShadowIsNoop) {
  std::vector<std::uint8_t> fb(static_cast<std::size_t>(4U) * 4U, 0x11);
  std::vector<std::uint8_t> shadow(4U, 0U);  // too small for a 2x2 fb
  compose_into_framebuffer(
      drm::span<std::uint8_t>(fb.data(), fb.size()), 8U, 2U, 2U, DRM_FORMAT_XRGB8888,
      drm::span<std::uint8_t>(shadow.data(), shadow.size()), drm::span<const FbBlitItem>{});
  EXPECT_EQ(fb[0], 0x11);  // untouched
}
