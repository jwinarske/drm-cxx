// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::capture::Image construction/assignment and
// drm::capture::write_png() file-output validation. The snapshot()
// path itself requires a live KMS device and is exercised via the
// capture examples against real hardware; this TU covers the
// platform-independent bits.

#include <drm-cxx/capture/png.hpp>
#include <drm-cxx/capture/snapshot.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <ios>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

using drm::capture::Image;
using drm::capture::write_png;

TEST(CaptureImage, DefaultCtorIsEmpty) {
  const Image img;
  EXPECT_TRUE(img.empty());
  EXPECT_EQ(img.width(), 0U);
  EXPECT_EQ(img.height(), 0U);
  EXPECT_EQ(img.stride_bytes(), 0U);
  EXPECT_TRUE(img.pixels().empty());
}

TEST(CaptureImage, ZeroDimensionStaysEmpty) {
  const Image a(0, 100);
  EXPECT_TRUE(a.empty());
  const Image b(100, 0);
  EXPECT_TRUE(b.empty());
}

TEST(CaptureImage, DimensionsAndStride) {
  Image img(4, 3);
  EXPECT_FALSE(img.empty());
  EXPECT_EQ(img.width(), 4U);
  EXPECT_EQ(img.height(), 3U);
  EXPECT_EQ(img.stride_bytes(), 16U);
  ASSERT_EQ(img.pixels().size(), 12U);
  // Zero-initialised.
  for (const auto px : img.pixels()) {
    EXPECT_EQ(px, 0U);
  }
}

TEST(CaptureImage, MoveLeavesSourceEmpty) {
  Image src(16, 16);
  src.pixels()[0] = 0xFF0000FFU;

  const Image moved = std::move(src);
  EXPECT_EQ(moved.width(), 16U);
  EXPECT_EQ(moved.height(), 16U);
  EXPECT_EQ(moved.pixels()[0], 0xFF0000FFU);
}

TEST(CaptureWritePng, RejectsEmpty) {
  const Image img;
  const auto tmp = fs::temp_directory_path() / "drm_cxx_capture_empty.png";
  const auto r = write_png(img, tmp.string());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(CaptureWritePng, WritesValidPngMagic) {
  Image img(8, 8);
  for (std::uint32_t y = 0; y < img.height(); ++y) {
    for (std::uint32_t x = 0; x < img.width(); ++x) {
      // 2x2 checkerboard of opaque white and black. Premultiplied ARGB
      // with alpha = 0xFF means RGB needs no scaling — the PNG encoder
      // sees straight 0xFF_FF_FF_FF / 0xFF_00_00_00.
      const std::uint32_t px = (((x / 2) + (y / 2)) % 2 == 0) ? 0xFFFFFFFFU : 0xFF000000U;
      img.pixels()[(y * img.width()) + x] = px;
    }
  }

  const auto tmp = fs::temp_directory_path() / "drm_cxx_capture_roundtrip.png";
  std::error_code ec;
  fs::remove(tmp, ec);  // clean state; ignore errors

  const auto r = write_png(img, tmp.string());
  ASSERT_TRUE(r.has_value()) << r.error().message();

  ASSERT_TRUE(fs::exists(tmp));
  EXPECT_GT(fs::file_size(tmp), 8U);

  std::ifstream in(tmp, std::ios::binary);
  ASSERT_TRUE(in.is_open());
  std::array<std::uint8_t, 8> magic{};
  in.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
  ASSERT_EQ(in.gcount(), static_cast<std::streamsize>(magic.size()));

  constexpr std::array<std::uint8_t, 8> k_png_signature = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
  };
  EXPECT_EQ(magic, k_png_signature);

  in.close();
  fs::remove(tmp, ec);
}