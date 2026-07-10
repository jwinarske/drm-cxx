// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::capture::write_jpg(). Covers the empty-image guard,
// the on-disk JPEG framing (SOI/EOI markers), quality clamping, and — the
// substantive check — a decode round-trip that confirms the encoder emits a
// valid baseline JPEG whose pixels match the source, including the
// premultiplied -> straight RGB conversion the encoder does on the way out.
//
// Only built when both Blend2D (capture::Image) and libjpeg are available;
// the test links libjpeg itself to decode what write_jpg produced.

#include <drm-cxx/capture/jpg.hpp>
#include <drm-cxx/capture/snapshot.hpp>

#include <array>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <ios>
#include <jpeglib.h>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using drm::capture::Image;
using drm::capture::write_jpg;

namespace {

// A single decoded RGB pixel plus the decoded geometry, or ok == false when
// the file could not be opened / decoded.
struct Decoded {
  bool ok{false};
  std::uint32_t w{0};
  std::uint32_t h{0};
  std::uint8_t r{0};
  std::uint8_t g{0};
  std::uint8_t b{0};
};

struct DecErr {
  jpeg_error_mgr pub;
  std::jmp_buf jb;
};

void dec_error_exit(j_common_ptr cinfo) {
  auto* mgr = reinterpret_cast<DecErr*>(cinfo->err);
  std::longjmp(mgr->jb, 1);
}

// Decode `path` and return the pixel at (x, y). Uses libjpeg's classic API so
// the test is independent of the encoder's internals.
Decoded decode_pixel(const std::string& path, std::uint32_t x, std::uint32_t y) {
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    return {};
  }

  jpeg_decompress_struct cinfo;
  DecErr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = dec_error_exit;
  std::vector<std::uint8_t> scan;
  Decoded out;
  if (setjmp(jerr.jb) != 0) {
    jpeg_destroy_decompress(&cinfo);
    (void)std::fclose(fp);
    return {};
  }

  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, fp);
  (void)jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = JCS_RGB;
  (void)jpeg_start_decompress(&cinfo);

  out.w = cinfo.output_width;
  out.h = cinfo.output_height;
  const std::uint32_t comps = static_cast<std::uint32_t>(cinfo.output_components);
  scan.resize(static_cast<std::size_t>(out.w) * comps);

  while (cinfo.output_scanline < cinfo.output_height) {
    const std::uint32_t line = cinfo.output_scanline;
    JSAMPROW rp = scan.data();
    (void)jpeg_read_scanlines(&cinfo, &rp, 1);
    if (line == y && x < out.w && comps >= 3U) {
      const std::size_t off = static_cast<std::size_t>(x) * comps;
      out.r = scan[off + 0];
      out.g = scan[off + 1];
      out.b = scan[off + 2];
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  (void)std::fclose(fp);
  out.ok = true;
  return out;
}

Image solid_image(std::uint32_t w, std::uint32_t h, std::uint32_t argb) {
  Image img(w, h);
  for (auto& px : img.pixels()) {
    px = argb;
  }
  return img;
}

}  // namespace

TEST(CaptureWriteJpg, RejectsEmpty) {
  const Image img;
  const auto tmp = fs::temp_directory_path() / "drm_cxx_capture_empty.jpg";
  const auto r = write_jpg(img, tmp.string());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(CaptureWriteJpg, WritesValidJpegFraming) {
  const Image img = solid_image(16, 16, 0xFF3366CCU);

  const auto tmp = fs::temp_directory_path() / "drm_cxx_capture_framing.jpg";
  std::error_code ec;
  fs::remove(tmp, ec);

  const auto r = write_jpg(img, tmp.string());
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_TRUE(fs::exists(tmp));
  const auto size = fs::file_size(tmp);
  ASSERT_GT(size, 4U);

  std::ifstream in(tmp, std::ios::binary);
  ASSERT_TRUE(in.is_open());
  std::vector<char> bytes(static_cast<std::size_t>(size));
  in.read(bytes.data(), static_cast<std::streamsize>(size));
  ASSERT_EQ(in.gcount(), static_cast<std::streamsize>(size));
  in.close();

  const auto u8 = [&](std::size_t i) { return static_cast<std::uint8_t>(bytes[i]); };
  // SOI marker at the start, EOI marker at the end.
  EXPECT_EQ(u8(0), 0xFFU);
  EXPECT_EQ(u8(1), 0xD8U);
  EXPECT_EQ(u8(2), 0xFFU);
  EXPECT_EQ(u8(bytes.size() - 2), 0xFFU);
  EXPECT_EQ(u8(bytes.size() - 1), 0xD9U);

  fs::remove(tmp, ec);
}

TEST(CaptureWriteJpg, RoundTripsOpaqueColor) {
  // Opaque premultiplied pixel: alpha 0xFF so RGB passes through unscaled.
  const Image img = solid_image(32, 32, 0xFF3366CCU);  // R=0x33 G=0x66 B=0xCC

  const auto tmp = fs::temp_directory_path() / "drm_cxx_capture_opaque.jpg";
  std::error_code ec;
  fs::remove(tmp, ec);
  ASSERT_TRUE(write_jpg(img, tmp.string(), 95).has_value());

  const Decoded d = decode_pixel(tmp.string(), 16, 16);
  ASSERT_TRUE(d.ok);
  EXPECT_EQ(d.w, 32U);
  EXPECT_EQ(d.h, 32U);
  // Baseline JPEG at q95 on a solid field is near-lossless; allow a small
  // quantization tolerance.
  EXPECT_NEAR(d.r, 0x33, 4);
  EXPECT_NEAR(d.g, 0x66, 4);
  EXPECT_NEAR(d.b, 0xCC, 4);

  fs::remove(tmp, ec);
}

TEST(CaptureWriteJpg, UnpremultipliesBeforeEncoding) {
  // Half-covered pixel: alpha 0x80, premultiplied channels already scaled by
  // ~0.5. The encoder must recover the straight color (~2x) before writing,
  // since JPEG has no alpha to carry the coverage.
  //   straight R = 0x40 * 255 / 0x80 = 0x7F (127.5 -> 128)
  const Image img = solid_image(32, 32, 0x80402010U);

  const auto tmp = fs::temp_directory_path() / "drm_cxx_capture_premul.jpg";
  std::error_code ec;
  fs::remove(tmp, ec);
  ASSERT_TRUE(write_jpg(img, tmp.string(), 95).has_value());

  const Decoded d = decode_pixel(tmp.string(), 16, 16);
  ASSERT_TRUE(d.ok);
  // 0x40/0x80 -> ~0x7F, 0x20/0x80 -> ~0x3F, 0x10/0x80 -> ~0x1F.
  EXPECT_NEAR(d.r, 0x7F, 5);
  EXPECT_NEAR(d.g, 0x3F, 5);
  EXPECT_NEAR(d.b, 0x1F, 5);

  fs::remove(tmp, ec);
}

TEST(CaptureWriteJpg, ClampsQualityOutOfRange) {
  const Image img = solid_image(8, 8, 0xFFFFFFFFU);
  const auto lo = fs::temp_directory_path() / "drm_cxx_capture_qlo.jpg";
  const auto hi = fs::temp_directory_path() / "drm_cxx_capture_qhi.jpg";
  std::error_code ec;

  EXPECT_TRUE(write_jpg(img, lo.string(), -5).has_value());
  EXPECT_TRUE(write_jpg(img, hi.string(), 500).has_value());
  EXPECT_TRUE(fs::exists(lo));
  EXPECT_TRUE(fs::exists(hi));

  fs::remove(lo, ec);
  fs::remove(hi, ec);
}
