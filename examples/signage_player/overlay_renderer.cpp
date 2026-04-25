// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "overlay_renderer.hpp"

// Blend2D's umbrella header layout varies by distro: Fedora ships
// /usr/include/blend2d/blend2d.h, some Debian packages ship the flat
// /usr/include/blend2d.h. Mirror src/capture/snapshot.cpp's probe so
// either reaches us, and so a clang-tidy pass that doesn't see the
// build system's -isystem path can still parse this TU as a no-op
// overlay (bg-only) without #error'ing out.
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#define SIGNAGE_OVERLAY_HAS_BLEND2D
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#define SIGNAGE_OVERLAY_HAS_BLEND2D
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace signage {

namespace {

// Straight-alpha 0xAARRGGBB → premultiplied 0xAARRGGBB. KMS scanout
// for ARGB8888 expects premultiplied; the playlist parser produces
// straight values from "#RRGGBBAA" literals, so the conversion is a
// boundary concern.
std::uint32_t premultiply(std::uint32_t straight) noexcept {
  const std::uint32_t a = (straight >> 24U) & 0xFFU;
  if (a == 0U) {
    return 0U;
  }
  if (a == 0xFFU) {
    return straight;
  }
  const std::uint32_t r = (((straight >> 16U) & 0xFFU) * a + 127U) / 255U;
  const std::uint32_t g = (((straight >> 8U) & 0xFFU) * a + 127U) / 255U;
  const std::uint32_t b = ((straight & 0xFFU) * a + 127U) / 255U;
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

#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D

// NOLINTBEGIN(misc-include-cleaner)

// Try a handful of well-known Linux font paths in priority order. fc-match
// would be the principled answer but pulls fontconfig as a hard dep for a
// scaffold-tier example; instead, hit the most common Fedora / Debian /
// Arch / Alpine layouts and bail if none load. Bold tends to read better
// on photographic backgrounds, so prefer Bold variants when available.
BLResult load_default_font_face(BLFontFace& face) noexcept {
  static constexpr std::array<const char*, 10> kCandidates = {
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
  for (const char* path : kCandidates) {
    if (face.create_from_file(path) == BL_SUCCESS) {
      return BL_SUCCESS;
    }
  }
  return BL_ERROR_FONT_NOT_INITIALIZED;
}

// Shared Blend2D draw path for the overlay's centered text and the
// clock's centered timestamp — same anchoring math, different inputs.
struct CenteredTextParams {
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t stride_bytes;
  std::uint32_t font_size;
  std::uint32_t fg_argb;
  std::string_view text;
};

void draw_centered_text_blend2d(drm::span<std::uint8_t> pixels,
                                const CenteredTextParams& p) noexcept {
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

  // tm.bounding_box is in design space relative to the baseline; width
  // is x1 - x0. Vertically center the inked-glyph box (ascent+descent)
  // inside the rect, then offset by ascent so fill_utf8_text's
  // baseline-anchored origin lands correctly.
  const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
  const double total_h = static_cast<double>(fm.ascent + fm.descent);
  const double x = (static_cast<double>(p.width) - text_w) * 0.5 - tm.bounding_box.x0;
  const double y = ((static_cast<double>(p.height) - total_h) * 0.5) + fm.ascent;

  BLContext ctx(img);
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
  ctx.fill_utf8_text(BLPoint(x, y), font, p.text.data(), p.text.size(), BLRgba32(p.fg_argb));
  ctx.end();
}

void draw_ticker_blend2d(drm::span<std::uint8_t> pixels, const TickerPaint& p) noexcept {
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

  // Per-pass advance: width of one rendered text plus a gap, so
  // consecutive copies don't run into each other. A gap of one font
  // size is the lazy choice that reads fine; if a playlist wants tighter
  // spacing it can append " · " or similar to the text itself.
  const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
  const double pass_w = text_w + static_cast<double>(p.font_size);
  if (pass_w <= 0.0) {
    return;
  }

  // Vertically centre the inked-glyph box, baseline-anchor as in the
  // overlay renderer.
  const double total_h = static_cast<double>(fm.ascent + fm.descent);
  const double y = ((static_cast<double>(p.height) - total_h) * 0.5) + fm.ascent;

  // Modulo the running scroll offset against pass_w so the buffer never
  // sees a discontinuity. Then start one pass to the left of the visible
  // region and stamp copies until we run past the right edge.
  const double scroll_mod = std::fmod(p.scroll_offset_px, pass_w);
  double x = -scroll_mod - tm.bounding_box.x0;

  BLContext ctx(img);
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
  while (x < static_cast<double>(p.width)) {
    ctx.fill_utf8_text(BLPoint(x, y), font, p.text.data(), p.text.size(), BLRgba32(p.fg_argb));
    x += pass_w;
  }
  ctx.end();
}

// NOLINTEND(misc-include-cleaner)

#endif  // SIGNAGE_OVERLAY_HAS_BLEND2D

}  // namespace

void paint_overlay(drm::span<std::uint8_t> pixels, const OverlayPaint& p) noexcept {
  if (p.width == 0U || p.height == 0U || p.stride_bytes < (p.width * 4U)) {
    return;
  }
  fill_premul(pixels, p.stride_bytes, p.width, p.height, premultiply(p.bg_argb));

#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D
  draw_centered_text_blend2d(pixels,
                             {p.width, p.height, p.stride_bytes, p.font_size, p.fg_argb, p.text});
#endif
}

void paint_ticker(drm::span<std::uint8_t> pixels, const TickerPaint& p) noexcept {
  if (p.width == 0U || p.height == 0U || p.stride_bytes < (p.width * 4U)) {
    return;
  }
  fill_premul(pixels, p.stride_bytes, p.width, p.height, premultiply(p.bg_argb));

#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D
  draw_ticker_blend2d(pixels, p);
#endif
}

void paint_clock(drm::span<std::uint8_t> pixels, const ClockPaint& p) noexcept {
  if (p.width == 0U || p.height == 0U || p.stride_bytes < (p.width * 4U)) {
    return;
  }
  fill_premul(pixels, p.stride_bytes, p.width, p.height, premultiply(p.bg_argb));

#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D
  draw_centered_text_blend2d(pixels,
                             {p.width, p.height, p.stride_bytes, p.font_size, p.fg_argb, p.text});
#endif
}

}  // namespace signage