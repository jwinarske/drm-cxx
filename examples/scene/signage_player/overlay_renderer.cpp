// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "overlay_renderer.hpp"

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/log.hpp>

// SIGNAGE_OVERLAY_HAS_BLEND2D is set by the build system iff the
// project's blend2d gate is on (see the example's CMakeLists.txt /
// meson.build). A bare __has_include probe is not enough: distros
// (Fedora, Arch) ship blend2d headers under /usr/include even when
// we did not link against blend2d, so the probe would silently pull
// the BL paths into the TU and the link would fail.
//
// The umbrella header layout varies by distro — Fedora ships
// /usr/include/blend2d/blend2d.h, some Debian packages ship the flat
// /usr/include/blend2d.h — so we still pick whichever the
// preprocessor can reach. A clang-tidy pass that runs without the
// build system's -isystem flags will see neither path, and the TU
// parses cleanly as a bg-only overlay.
#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#else
// Build system promised blend2d is linked but no header reaches us.
#undef SIGNAGE_OVERLAY_HAS_BLEND2D
#endif
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

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
  const std::uint32_t r = ((((straight >> 16U) & 0xFFU) * a) + 127U) / 255U;
  const std::uint32_t g = ((((straight >> 8U) & 0xFFU) * a) + 127U) / 255U;
  const std::uint32_t b = (((straight & 0xFFU) * a) + 127U) / 255U;
  return (a << 24U) | (r << 16U) | (g << 8U) | b;
}

void fill_premul(drm::span<std::uint8_t> pixels, std::uint32_t stride_bytes, std::uint32_t width,
                 std::uint32_t height, std::uint32_t premul) noexcept {
  // Caller has already validated width*4 <= stride_bytes and the span size
  // (see the paint_* entry points). The cast assumes 4-byte alignment, which
  // every dumb/GBM scanout buffer satisfies (page-aligned mmap origin).
  const std::uint32_t stride_px = stride_bytes / 4U;
  auto* px = reinterpret_cast<std::uint32_t*>(pixels.data());
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row = px + (static_cast<std::size_t>(y) * stride_px);
    std::fill_n(row, width, premul);
  }
}

// Common entry guard for paint_*: rejects degenerate dims, narrow strides,
// and (most importantly) any span that cannot hold height * stride_bytes
// of pixel data — guards against a misshapen caller writing OOB.
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

#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D

// NOLINTBEGIN(misc-include-cleaner)

// Try a handful of well-known Linux font paths in priority order. fc-match
// would be the principled answer but pulls fontconfig as a hard dep for a
// scaffold-tier example; instead, hit the most common Fedora / Debian /
// Arch / Alpine layouts and bail if none load. Bold tends to read better
// on photographic backgrounds, so prefer Bold variants when available.
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

  // tm.bounding_box is in the design space relative to the baseline; width
  // is x1 - x0. Vertically center the inked-glyph box (ascent+descent)
  // inside the rect, then offset by ascent so fill_utf8_text's
  // baseline-anchored origin lands correctly.
  const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
  const auto total_h = static_cast<double>(fm.ascent + fm.descent);
  const double x = ((static_cast<double>(p.width) - text_w) * 0.5) - tm.bounding_box.x0;
  const double y = ((static_cast<double>(p.height) - total_h) * 0.5) + fm.ascent;

  BLContext ctx(img);
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
  ctx.fill_utf8_text(BLPoint(x, y), font, p.text.data(), p.text.size(), BLRgba32(p.fg_argb));
  ctx.end();
}

void draw_logo_blend2d(drm::span<std::uint8_t> pixels, const LogoPaint& p) noexcept {
  if (p.path.empty()) {
    return;
  }
  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(p.width), static_cast<int>(p.height),
                              BL_FORMAT_PRGB32, pixels.data(),
                              static_cast<intptr_t>(p.stride_bytes), BL_DATA_ACCESS_RW, nullptr,
                              nullptr) != BL_SUCCESS) {
    return;
  }

  // BLImage::read_from_file needs a C string; the LogoPaint carries a
  // string_view to keep the struct lightweight, so copy here. Logo is
  // single-shot anyway — the per-call allocation is in the noise.
  const std::string path_z(p.path);
  BLImage src;
  if (src.read_from_file(path_z.c_str()) != BL_SUCCESS) {
    // Most common cause: relative path doesn't resolve against the
    // current working directory. Emit a warning so the failure
    // isn't invisible — the layer keeps the fallback fill, so the
    // rest of the scene is unaffected.
    drm::log_warn(
        "signage_player: logo PNG load failed for path '{}' — check the playlist's "
        "[logo].path is reachable from the current working directory or use an absolute path",
        path_z);
    return;
  }

  const int sw = src.width();
  const int sh = src.height();
  if (sw <= 0 || sh <= 0) {
    return;
  }

  // Scale-to-fit while preserving aspect, letterboxing the unused
  // pixels with the fallback fill that's already underneath.
  const double scale = std::min(static_cast<double>(p.width) / static_cast<double>(sw),
                                static_cast<double>(p.height) / static_cast<double>(sh));
  const int dw = static_cast<int>(static_cast<double>(sw) * scale);
  const int dh = static_cast<int>(static_cast<double>(sh) * scale);
  const int dx = (static_cast<int>(p.width) - dw) / 2;
  const int dy = (static_cast<int>(p.height) - dh) / 2;

  BLContext ctx(canvas);
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
  ctx.blit_image(BLRectI(dx, dy, dw, dh), src);
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
  const auto total_h = static_cast<double>(fm.ascent + fm.descent);
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
  if (!valid_target(pixels, p.width, p.height, p.stride_bytes)) {
    return;
  }
  fill_premul(pixels, p.stride_bytes, p.width, p.height, premultiply(p.bg_argb));

#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D
  draw_centered_text_blend2d(pixels,
                             {p.width, p.height, p.stride_bytes, p.font_size, p.fg_argb, p.text});
#endif
}

void paint_ticker(drm::span<std::uint8_t> pixels, const TickerPaint& p) noexcept {
  if (!valid_target(pixels, p.width, p.height, p.stride_bytes)) {
    return;
  }
  fill_premul(pixels, p.stride_bytes, p.width, p.height, premultiply(p.bg_argb));

#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D
  draw_ticker_blend2d(pixels, p);
#endif
}

void paint_clock(drm::span<std::uint8_t> pixels, const ClockPaint& p) noexcept {
  if (!valid_target(pixels, p.width, p.height, p.stride_bytes)) {
    return;
  }
  fill_premul(pixels, p.stride_bytes, p.width, p.height, premultiply(p.bg_argb));

#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D
  draw_centered_text_blend2d(pixels,
                             {p.width, p.height, p.stride_bytes, p.font_size, p.fg_argb, p.text});
#endif
}

void paint_logo(drm::span<std::uint8_t> pixels, const LogoPaint& p) noexcept {
  if (!valid_target(pixels, p.width, p.height, p.stride_bytes)) {
    return;
  }
  // Pre-fill so the layer is visible (and any letterbox margins are
  // covered) when the PNG load below succeeds, fails, or Blend2D is
  // absent.
  fill_premul(pixels, p.stride_bytes, p.width, p.height, premultiply(p.fallback_argb));

#ifdef SIGNAGE_OVERLAY_HAS_BLEND2D
  draw_logo_blend2d(pixels, p);
#endif
}

}  // namespace signage