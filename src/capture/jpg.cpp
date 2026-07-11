// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "jpg.hpp"

// libjpeg (satisfied by libjpeg-turbo) is optional. Mirror png.cpp's degrade-
// to-empty-TU guard: a tidy pass that parses this file without libjpeg on the
// include path sees an empty TU and stays green, even though the file is only
// ever *built* when DRM_CXX_HAS_LIBJPEG is set. A real build with the dep
// missing fails loudly at link time instead. The concrete-symbol includes the
// body needs live inside the same guard so they never read as unused when the
// body is compiled out.
#if __has_include(<jpeglib.h>)
// <cstdio> must precede <jpeglib.h>: libjpeg's public header references FILE
// (for jpeg_stdio_dest) but does not include <stdio.h> itself.
#include <cstdio>  // NOLINT(misc-include-cleaner) — ordering dep for <jpeglib.h>
#include <jpeglib.h>
#define DRM_CXX_CAPTURE_HAS_JPEG
#endif

#ifdef DRM_CXX_CAPTURE_HAS_JPEG
#include "detail/expected.hpp"
#include "detail/span.hpp"
#include "snapshot.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#endif

namespace drm::capture {

#ifdef DRM_CXX_CAPTURE_HAS_JPEG

namespace {

// libjpeg's default error handler calls exit() on a fatal error. Replace it
// with one that longjmps back to write_jpg's setjmp point so a bad encode
// becomes an error return instead of taking the process down. `pub` must be
// first so the jpeg_error_mgr* libjpeg hands us aliases the enclosing struct.
struct JpgErrorMgr {
  jpeg_error_mgr pub;
  std::jmp_buf jb;
};

void jpg_error_exit(j_common_ptr cinfo) {
  auto* mgr = reinterpret_cast<JpgErrorMgr*>(cinfo->err);
  std::longjmp(mgr->jb, 1);
}

// Un-premultiply one channel: recover the straight (non-premultiplied) value
// `c` had before it was scaled by coverage `a`. Opaque pixels (a == 255) pass
// through unchanged, which is the common screenshot case; a == 0 is fully
// transparent, so the color is undefined — emit black.
std::uint8_t unpremultiply(std::uint32_t c, std::uint32_t a) {
  if (a == 0U) {
    return 0U;
  }
  if (a >= 255U) {
    return static_cast<std::uint8_t>(c);
  }
  const std::uint32_t v = (c * 255U + a / 2U) / a;
  return static_cast<std::uint8_t>(v > 255U ? 255U : v);
}

}  // namespace

drm::expected<void, std::error_code> write_jpg(const Image& image, std::string_view path,
                                               int quality) {
  if (image.empty()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  const int q = quality < 1 ? 1 : quality;
  const int quality_clamped = q > 100 ? 100 : q;
  const std::uint32_t width = image.width();
  const std::uint32_t height = image.height();

  // Everything read in the setjmp error branch below (path string, file
  // handle, scanline scratch) is established *before* setjmp so a longjmp
  // never observes an indeterminate automatic.
  const std::string path_z(path);
  std::FILE* fp = std::fopen(path_z.c_str(), "wb");
  if (fp == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::generic_category()));
  }
  std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3U);

  jpeg_compress_struct cinfo{};
  JpgErrorMgr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpg_error_exit;
  if (setjmp(jerr.jb) != 0) {
    jpeg_destroy_compress(&cinfo);
    (void)std::fclose(fp);
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::io_error));
  }

  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality_clamped, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  const drm::span<const std::uint32_t> px = image.pixels();
  while (cinfo.next_scanline < cinfo.image_height) {
    const std::size_t base = static_cast<std::size_t>(cinfo.next_scanline) * width;
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint32_t p = px[base + x];
      const std::uint32_t a = (p >> 24) & 0xFFU;
      row[(static_cast<std::size_t>(x) * 3U) + 0U] = unpremultiply((p >> 16) & 0xFFU, a);  // R
      row[(static_cast<std::size_t>(x) * 3U) + 1U] = unpremultiply((p >> 8) & 0xFFU, a);   // G
      row[(static_cast<std::size_t>(x) * 3U) + 2U] = unpremultiply(p & 0xFFU, a);          // B
    }
    JSAMPROW rp = row.data();
    (void)jpeg_write_scanlines(&cinfo, &rp, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  (void)std::fclose(fp);
  return {};
}

drm::expected<void, std::error_code> write_jpg_nv12(const std::uint8_t* y, const std::uint8_t* uv,
                                                    std::uint32_t y_stride, std::uint32_t uv_stride,
                                                    std::uint32_t width, std::uint32_t height,
                                                    std::string_view path, int quality) {
  if (y == nullptr || uv == nullptr || width == 0U || height == 0U) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  const int q = quality < 1 ? 1 : quality;
  const int quality_clamped = q > 100 ? 100 : q;

  // 4:2:0 chroma grid: one Cb/Cr sample per 2x2 luma block (round up so an odd
  // edge column/row still gets a sample). libjpeg's raw path wants the two
  // chroma channels as separate planes, so de-interleave NV12's Cb,Cr once.
  const std::uint32_t cw = (width + 1U) / 2U;
  const std::uint32_t ch = (height + 1U) / 2U;
  std::vector<std::uint8_t> cb(static_cast<std::size_t>(cw) * ch);
  std::vector<std::uint8_t> cr(static_cast<std::size_t>(cw) * ch);
  for (std::uint32_t r = 0; r < ch; ++r) {
    const std::uint8_t* row = uv + (static_cast<std::size_t>(r) * uv_stride);
    for (std::uint32_t c = 0; c < cw; ++c) {
      const std::size_t src = static_cast<std::size_t>(c) * 2U;
      const std::size_t dst = (static_cast<std::size_t>(r) * cw) + c;
      cb[dst] = row[src];
      cr[dst] = row[src + 1U];
    }
  }

  const std::string path_z(path);
  std::FILE* fp = std::fopen(path_z.c_str(), "wb");
  if (fp == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::generic_category()));
  }

  jpeg_compress_struct cinfo{};
  JpgErrorMgr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpg_error_exit;
  if (setjmp(jerr.jb) != 0) {
    jpeg_destroy_compress(&cinfo);
    (void)std::fclose(fp);
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::io_error));
  }

  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_YCbCr;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality_clamped, TRUE);
  // Y at full resolution (2x2), Cb/Cr subsampled (1x1) — i.e. 4:2:0.
  cinfo.comp_info[0].h_samp_factor = 2;
  cinfo.comp_info[0].v_samp_factor = 2;
  cinfo.comp_info[1].h_samp_factor = 1;
  cinfo.comp_info[1].v_samp_factor = 1;
  cinfo.comp_info[2].h_samp_factor = 1;
  cinfo.comp_info[2].v_samp_factor = 1;
  cinfo.raw_data_in = TRUE;
  jpeg_start_compress(&cinfo, TRUE);

  // Feed one iMCU row per call: 16 luma lines + 8 chroma lines. The last row
  // is edge-padded (extra pointers alias the final valid line); libjpeg crops
  // the output back to image_height.
  const int y_lines = DCTSIZE * cinfo.comp_info[0].v_samp_factor;  // 16
  const int c_lines = DCTSIZE * cinfo.comp_info[1].v_samp_factor;  // 8
  std::vector<JSAMPROW> y_rows(static_cast<std::size_t>(y_lines));
  std::vector<JSAMPROW> cb_rows(static_cast<std::size_t>(c_lines));
  std::vector<JSAMPROW> cr_rows(static_cast<std::size_t>(c_lines));
  std::array<JSAMPARRAY, 3> planes{y_rows.data(), cb_rows.data(), cr_rows.data()};

  while (cinfo.next_scanline < cinfo.image_height) {
    const std::uint32_t base = cinfo.next_scanline;
    for (int i = 0; i < y_lines; ++i) {
      const std::uint32_t line = std::min(base + static_cast<std::uint32_t>(i), height - 1U);
      const std::uint8_t* row = y + (static_cast<std::size_t>(line) * y_stride);
      // libjpeg reads (never writes) the input during compress; shed const.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      y_rows[static_cast<std::size_t>(i)] = const_cast<JSAMPROW>(row);
    }
    const std::uint32_t cbase = base / 2U;
    for (int i = 0; i < c_lines; ++i) {
      const std::uint32_t line = std::min(cbase + static_cast<std::uint32_t>(i), ch - 1U);
      cb_rows[static_cast<std::size_t>(i)] = cb.data() + (static_cast<std::size_t>(line) * cw);
      cr_rows[static_cast<std::size_t>(i)] = cr.data() + (static_cast<std::size_t>(line) * cw);
    }
    (void)jpeg_write_raw_data(&cinfo, planes.data(), static_cast<JDIMENSION>(y_lines));
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  (void)std::fclose(fp);
  return {};
}

#endif  // DRM_CXX_CAPTURE_HAS_JPEG

}  // namespace drm::capture
