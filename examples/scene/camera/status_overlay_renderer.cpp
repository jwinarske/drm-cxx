// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "status_overlay_renderer.hpp"

#include <drm-cxx/detail/span.hpp>

// CAMERA_STATUS_HAS_BLEND2D is set by the build system iff the project's
// blend2d gate is on. A bare __has_include probe is not enough: distros
// (Fedora, Arch) ship blend2d headers under /usr/include even when we did
// not link against blend2d, so the probe would silently pull the BL paths
// into the TU and the link would fail.
#ifdef CAMERA_STATUS_HAS_BLEND2D
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#else
#undef CAMERA_STATUS_HAS_BLEND2D
#endif
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace camera {

namespace {

std::uint32_t premultiply(std::uint32_t straight) noexcept {
  const std::uint32_t a = (straight >> 24U) & 0xFFU;
  if (a == 0U) {
    return 0U;
  }
  if (a == 0xFFU) {
    return straight;
  }
  const std::uint32_t r = ((((straight >> 16U) & 0xFFU) * a) + 127U) / 255U;
  const std::uint32_t g = ((((straight >> 8U) & 0xFFU) * a) + 127U) / 255U;
  const std::uint32_t b = (((straight & 0xFFU) * a) + 127U) / 255U;
  return (a << 24U) | (r << 16U) | (g << 8U) | b;
}

void fill_premul(drm::span<std::uint8_t> pixels, std::uint32_t stride_bytes, std::uint32_t width,
                 std::uint32_t height, std::uint32_t premul) noexcept {
  const std::uint32_t stride_px = stride_bytes / 4U;
  auto* px = reinterpret_cast<std::uint32_t*>(pixels.data());
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row = px + (static_cast<std::size_t>(y) * stride_px);
    std::fill_n(row, width, premul);
  }
}

[[nodiscard]] bool valid_target(drm::span<const std::uint8_t> pixels, std::uint32_t width,
                                std::uint32_t height, std::uint32_t stride_bytes) noexcept {
  if (width == 0U || height == 0U) {
    return false;
  }
  if (stride_bytes < (width * 4U)) {
    return false;
  }
  const std::size_t required = static_cast<std::size_t>(height) * stride_bytes;
  return pixels.size() >= required;
}

#ifdef CAMERA_STATUS_HAS_BLEND2D

// NOLINTBEGIN(misc-include-cleaner)

BLResult load_default_font_face(BLFontFace& face) noexcept {
  static constexpr std::array<const char*, 10> k_candidates = {
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf",
      "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/TTF/Vera.ttf",
  };
  for (const char* path : k_candidates) {
    if (face.create_from_file(path) == BL_SUCCESS) {
      return BL_SUCCESS;
    }
  }
  return BL_ERROR_FONT_NOT_INITIALIZED;
}

void draw_centered_text(drm::span<std::uint8_t> pixels, const StatusPaint& p) noexcept {
  if (p.text.empty()) {
    return;
  }
  BLImage img;
  if (img.create_from_data(static_cast<int>(p.width), static_cast<int>(p.height), BL_FORMAT_PRGB32,
                           pixels.data(), static_cast<intptr_t>(p.stride_bytes), BL_DATA_ACCESS_RW,
                           nullptr, nullptr) != BL_SUCCESS) {
    return;
  }
  BLFontFace face;
  if (load_default_font_face(face) != BL_SUCCESS) {
    return;
  }
  BLFont font;
  if (font.create_from_face(face, static_cast<float>(p.font_size)) != BL_SUCCESS) {
    return;
  }
  BLGlyphBuffer gb;
  gb.set_utf8_text(p.text.data(), p.text.size());
  if (font.shape(gb) != BL_SUCCESS) {
    return;
  }
  BLTextMetrics tm{};
  if (font.get_text_metrics(gb, tm) != BL_SUCCESS) {
    return;
  }
  const BLFontMetrics fm = font.metrics();

  const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
  const auto total_h = static_cast<double>(fm.ascent + fm.descent);
  const double x = ((static_cast<double>(p.width) - text_w) * 0.5) - tm.bounding_box.x0;
  const double y = ((static_cast<double>(p.height) - total_h) * 0.5) + fm.ascent;

  BLContext ctx(img);
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
  ctx.fill_utf8_text(BLPoint(x, y), font, p.text.data(), p.text.size(), BLRgba32(p.fg_argb));
  ctx.end();
}

// NOLINTEND(misc-include-cleaner)

#endif  // CAMERA_STATUS_HAS_BLEND2D

}  // namespace

void paint_status(drm::span<std::uint8_t> pixels, const StatusPaint& p) noexcept {
  if (!valid_target(pixels, p.width, p.height, p.stride_bytes)) {
    return;
  }
  fill_premul(pixels, p.stride_bytes, p.width, p.height, premultiply(p.bg_argb));
#ifdef CAMERA_STATUS_HAS_BLEND2D
  draw_centered_text(pixels, p);
#endif
}

}  // namespace camera