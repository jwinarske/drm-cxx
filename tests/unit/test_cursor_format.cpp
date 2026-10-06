// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// The cursor plane's pixel format (cursor/cursor_format.hpp): which plane
// formats a cursor accepts, and how an ARGB8888 pixel lands in each.

#include "cursor/cursor_format.hpp"
#include "planes/plane_registry.hpp"

#include <drm_fourcc.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

using drm::cursor::detail::cursor_format;
using drm::cursor::detail::from_argb;

namespace {

drm::planes::PlaneCapabilities plane_with(std::vector<std::uint32_t> formats) {
  drm::planes::PlaneCapabilities caps;
  caps.type = drm::planes::DRMPlaneType::CURSOR;
  caps.formats = std::move(formats);
  return caps;
}

// The pixel as the plane reads it: bytes in memory, lowest address first.
std::array<std::uint8_t, 4> bytes(std::uint32_t word) {
  std::array<std::uint8_t, 4> out{};
  std::memcpy(out.data(), &word, out.size());
  return out;
}

}  // namespace

// Tegra's cursor plane advertises only RGBA8888: accepted.
TEST(CursorFormat, AcceptsRgbaOnlyPlane) {
  EXPECT_EQ(cursor_format(plane_with({DRM_FORMAT_RGBA8888})), DRM_FORMAT_RGBA8888);
}

// ARGB8888 wins whenever it is there, so the common case is unchanged.
TEST(CursorFormat, PrefersArgb) {
  EXPECT_EQ(
      cursor_format(plane_with({DRM_FORMAT_BGRA8888, DRM_FORMAT_RGBA8888, DRM_FORMAT_ARGB8888})),
      DRM_FORMAT_ARGB8888);
  EXPECT_EQ(cursor_format(plane_with({DRM_FORMAT_BGRA8888, DRM_FORMAT_ABGR8888})),
            DRM_FORMAT_ABGR8888);
}

// A cursor needs alpha: opaque formats never qualify.
TEST(CursorFormat, RejectsOpaqueFormats) {
  EXPECT_FALSE(
      cursor_format(plane_with({DRM_FORMAT_XRGB8888, DRM_FORMAT_RGBX8888, DRM_FORMAT_XBGR8888,
                                DRM_FORMAT_BGRX8888, DRM_FORMAT_RGB565}))
          .has_value());
}

// One premultiplied half-transparent pixel (A=0x80 R=0x11 G=0x22 B=0x33),
// checked as bytes in memory: a word-level check passes on a permutation that
// lays the bytes down wrong.
TEST(CursorFormat, PixelBytesPerFormat) {
  constexpr std::uint32_t k_argb = 0x80112233U;
  // DRM fourcc formats are little-endian words.
  EXPECT_EQ(bytes(from_argb(k_argb, DRM_FORMAT_ARGB8888)),
            (std::array<std::uint8_t, 4>{0x33, 0x22, 0x11, 0x80}));  // B G R A
  EXPECT_EQ(bytes(from_argb(k_argb, DRM_FORMAT_RGBA8888)),
            (std::array<std::uint8_t, 4>{0x80, 0x33, 0x22, 0x11}));  // A B G R
  EXPECT_EQ(bytes(from_argb(k_argb, DRM_FORMAT_ABGR8888)),
            (std::array<std::uint8_t, 4>{0x11, 0x22, 0x33, 0x80}));  // R G B A
  EXPECT_EQ(bytes(from_argb(k_argb, DRM_FORMAT_BGRA8888)),
            (std::array<std::uint8_t, 4>{0x80, 0x11, 0x22, 0x33}));  // A R G B
}
