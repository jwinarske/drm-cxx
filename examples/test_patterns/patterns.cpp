// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "patterns.hpp"

#include <drm-cxx/detail/span.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace drm::examples::test_patterns {

namespace {

constexpr std::uint32_t pack(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
  return (static_cast<std::uint32_t>(r) << 16U) | (static_cast<std::uint32_t>(g) << 8U) |
         static_cast<std::uint32_t>(b);
}

constexpr std::uint32_t k_black = pack(0, 0, 0);
constexpr std::uint32_t k_white = pack(255, 255, 255);

// SMPTE 75 % code = 0.75 * 255 ≈ 191. Centralized so the bar palette
// reads as combinations of "75 %" channels rather than raw 191 literals.
constexpr std::uint8_t k_75 = 191;

// Row pointer into the buffer at scanline `y`. Caller has already
// validated that the span holds height * stride_bytes; pointer
// arithmetic stays in bounds. Not constexpr — reinterpret_cast can't
// be evaluated at compile time.
std::uint32_t* row_ptr(const PaintTarget& t, std::uint32_t y) noexcept {
  // Dumb-buffer mappings are page-aligned, so the cast to uint32_t* is
  // safe in practice. The detail/span guarantees a contiguous range.
  auto* base = reinterpret_cast<std::uint32_t*>(t.pixels.data());
  const std::uint32_t stride_px = t.stride_bytes / 4U;
  return base + (static_cast<std::size_t>(y) * stride_px);
}

void fill_rows(const PaintTarget& t, std::uint32_t y0, std::uint32_t y1,
               std::uint32_t color) noexcept {
  for (std::uint32_t y = y0; y < y1; ++y) {
    auto* row = row_ptr(t, y);
    std::fill_n(row, t.width, color);
  }
}

void fill_rect(const PaintTarget& t, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
               std::uint32_t y1, std::uint32_t color) noexcept {
  const std::uint32_t cx0 = std::min(x0, t.width);
  const std::uint32_t cx1 = std::min(x1, t.width);
  const std::uint32_t cy0 = std::min(y0, t.height);
  const std::uint32_t cy1 = std::min(y1, t.height);
  if (cx1 <= cx0 || cy1 <= cy0) {
    return;
  }
  for (std::uint32_t y = cy0; y < cy1; ++y) {
    auto* row = row_ptr(t, y) + cx0;
    std::fill_n(row, cx1 - cx0, color);
  }
}

[[nodiscard]] bool valid_target(const PaintTarget& t) noexcept {
  if (t.width == 0U || t.height == 0U || t.stride_bytes < (t.width * 4U)) {
    return false;
  }
  const std::size_t required = static_cast<std::size_t>(t.height) * t.stride_bytes;
  return t.pixels.size() >= required;
}

// ── Pattern 1: SMPTE 75 % bars + PLUGE-style sub-bands ─────────────────
//
// Three bands stacked top-to-bottom. Bar widths use floor division; the
// rightmost bar absorbs the remainder so the right edge always meets the
// frame boundary exactly.
void paint_smpte_bars(const PaintTarget& t) noexcept {
  // Top band — 7 colour bars at 75 % intensity. Order matches RP 219
  // top-band convention (gray, Y, C, G, M, R, B), descending by Y'.
  static constexpr std::array<std::uint32_t, 7> k_top_bars = {
      pack(k_75, k_75, k_75),  // 75 % gray
      pack(k_75, k_75, 0),     // 75 % yellow
      pack(0, k_75, k_75),     // 75 % cyan
      pack(0, k_75, 0),        // 75 % green
      pack(k_75, 0, k_75),     // 75 % magenta
      pack(k_75, 0, 0),        // 75 % red
      pack(0, 0, k_75),        // 75 % blue
  };

  // Reverse-blue band — alternates the chroma component under the top
  // band so a chroma-decode mismatch is immediately visible.
  static constexpr std::array<std::uint32_t, 7> k_mid_bars = {
      pack(0, 0, k_75),        // blue
      k_black,                 //
      pack(k_75, 0, k_75),     // magenta
      k_black,                 //
      pack(0, k_75, k_75),     // cyan
      k_black,                 //
      pack(k_75, k_75, k_75),  // 75 % gray
  };

  const std::uint32_t top_h = (t.height * 2U) / 3U;
  const std::uint32_t mid_h = t.height * 3U / 4U;  // mid runs top_h .. mid_h
  const std::uint32_t bar_w = t.width / 7U;

  // Top + middle bars. Iterate via pointer arithmetic on the array's
  // begin() so the bounds-check tidy lint stays happy without forcing
  // a runtime-checked .at() into the hot path.
  const auto* top_it = k_top_bars.begin();
  const auto* mid_it = k_mid_bars.begin();
  for (std::uint32_t i = 0; i < 7U; ++i, ++top_it, ++mid_it) {
    const std::uint32_t x0 = i * bar_w;
    const std::uint32_t x1 = (i == 6U) ? t.width : (x0 + bar_w);
    fill_rect(t, x0, 0, x1, top_h, *top_it);
    fill_rect(t, x0, top_h, x1, mid_h, *mid_it);
  }

  // PLUGE band — divide the bottom strip into five blocks so the layout
  // mirrors the canonical pattern: -I, white, +Q, black-with-pulses,
  // white. Block widths sum to t.width with the last block absorbing
  // any remainder.
  const std::uint32_t pluge_w0 = t.width / 6U;   // -I block
  const std::uint32_t pluge_w1 = t.width / 12U;  // left white
  const std::uint32_t pluge_w2 = t.width / 6U;   // +Q block
  const std::uint32_t pluge_w4 = t.width / 12U;  // right white
  // pluge_w3 = remainder, computed below.

  std::uint32_t cursor = 0;
  fill_rect(t, cursor, mid_h, cursor + pluge_w0, t.height, pack(0, 0, 76));  // -I
  cursor += pluge_w0;
  fill_rect(t, cursor, mid_h, cursor + pluge_w1, t.height, k_white);
  cursor += pluge_w1;
  fill_rect(t, cursor, mid_h, cursor + pluge_w2, t.height, pack(72, 0, 130));  // +Q
  cursor += pluge_w2;
  const std::uint32_t pluge_x_white_right = (t.width > pluge_w4) ? (t.width - pluge_w4) : t.width;
  // Black surround takes everything between cursor and pluge_x_white_right.
  fill_rect(t, cursor, mid_h, pluge_x_white_right, t.height, k_black);

  // PLUGE pulses inside the black surround. Three thin vertical stripes
  // at +2 %, 0 %, +4 % — the -2 % bar is omitted because an 8-bit
  // framebuffer cannot represent sub-zero luminance and would clip to 0.
  const std::uint32_t pulse_w = std::max(1U, t.width / 60U);
  const std::uint32_t pulse_gap = std::max(1U, t.width / 80U);
  const std::uint32_t pulses_total = (3U * pulse_w) + (2U * pulse_gap);
  if (pluge_x_white_right > cursor + pulses_total) {
    const std::uint32_t pulses_x0 = cursor + ((pluge_x_white_right - cursor - pulses_total) / 2U);
    static constexpr std::array<std::uint8_t, 3> k_pulse_codes = {5, 0, 10};
    std::uint32_t px = pulses_x0;
    for (const auto code : k_pulse_codes) {
      fill_rect(t, px, mid_h, px + pulse_w, t.height, pack(code, code, code));
      px += pulse_w + pulse_gap;
    }
  }

  fill_rect(t, pluge_x_white_right, mid_h, t.width, t.height, k_white);
}

// ── Pattern 2: 1-pixel pitch B/W vertical stripes ──────────────────────
//
// Maximum spatial frequency the panel can resolve. On LCDs this also
// drives every column line at peak duty cycle.
void paint_pixel_stripes(const PaintTarget& t) noexcept {
  for (std::uint32_t y = 0; y < t.height; ++y) {
    auto* row = row_ptr(t, y);
    for (std::uint32_t x = 0; x < t.width; ++x) {
      row[x] = ((x & 1U) == 0U) ? k_white : k_black;
    }
  }
}

// ── Pattern 3: 11-step gray bars ───────────────────────────────────────
//
// 0 %, 10 %, 20 %, …, 100 %. Reveals gamma response and posterization.
void paint_gray_bars(const PaintTarget& t) noexcept {
  static constexpr std::array<std::uint8_t, 11> k_codes = {0,   26,  51,  77,  102, 128,
                                                           153, 179, 204, 230, 255};
  const std::uint32_t bar_w = t.width / 11U;
  std::uint32_t i = 0;
  for (const auto code : k_codes) {
    const std::uint32_t x0 = i * bar_w;
    const std::uint32_t x1 = (i == 10U) ? t.width : (x0 + bar_w);
    fill_rect(t, x0, 0, x1, t.height, pack(code, code, code));
    ++i;
  }
}

// ── Pattern 4: 64 px checkerboard ──────────────────────────────────────
constexpr std::uint32_t k_checker_tile = 64;

void paint_checkerboard(const PaintTarget& t) noexcept {
  for (std::uint32_t y = 0; y < t.height; ++y) {
    auto* row = row_ptr(t, y);
    const std::uint32_t ty = y / k_checker_tile;
    for (std::uint32_t x = 0; x < t.width; ++x) {
      const std::uint32_t tx = x / k_checker_tile;
      row[x] = (((tx + ty) & 1U) == 0U) ? k_black : k_white;
    }
  }
}

// ── Pattern 5: continuous gray gradient ────────────────────────────────
void paint_gray_gradient(const PaintTarget& t) noexcept {
  const std::uint32_t denom = (t.width > 1U) ? (t.width - 1U) : 1U;
  for (std::uint32_t y = 0; y < t.height; ++y) {
    auto* row = row_ptr(t, y);
    for (std::uint32_t x = 0; x < t.width; ++x) {
      const auto g = static_cast<std::uint8_t>((x * 255U) / denom);
      row[x] = pack(g, g, g);
    }
  }
}

// ── Pattern 6: stacked R/G/B horizontal gradients ──────────────────────
void paint_color_gradient(const PaintTarget& t) noexcept {
  const std::uint32_t band_h = t.height / 3U;
  const std::uint32_t denom = (t.width > 1U) ? (t.width - 1U) : 1U;
  // Pre-fill the bottom band's slack rows with black before painting
  // the three colour bands so the rounding remainder rows aren't left
  // at the zero-init colour they were already at — the buffer is zero-
  // filled on creation but a re-paint over a previous pattern wouldn't be.
  fill_rows(t, 0, t.height, k_black);
  for (std::uint32_t y = 0; y < band_h; ++y) {
    auto* row_r = row_ptr(t, y);
    auto* row_g = row_ptr(t, y + band_h);
    auto* row_b = row_ptr(t, y + (2U * band_h));
    for (std::uint32_t x = 0; x < t.width; ++x) {
      const auto c = static_cast<std::uint8_t>((x * 255U) / denom);
      row_r[x] = pack(c, 0, 0);
      row_g[x] = pack(0, c, 0);
      row_b[x] = pack(0, 0, c);
    }
  }
}

// ── Pattern 7: cross-hatch grid ────────────────────────────────────────
constexpr std::uint32_t k_hatch_pitch = 64;

void paint_crosshatch(const PaintTarget& t) noexcept {
  fill_rows(t, 0, t.height, k_black);
  // Horizontal lines every k_hatch_pitch rows.
  for (std::uint32_t y = 0; y < t.height; y += k_hatch_pitch) {
    auto* row = row_ptr(t, y);
    std::fill_n(row, t.width, k_white);
  }
  // Vertical lines every k_hatch_pitch columns.
  for (std::uint32_t y = 0; y < t.height; ++y) {
    auto* row = row_ptr(t, y);
    for (std::uint32_t x = 0; x < t.width; x += k_hatch_pitch) {
      row[x] = k_white;
    }
  }
}

// ── Pattern 8: 5×5 procedural "H" glyph grid ───────────────────────────
//
// Procedural so the example doesn't drag in a font dependency. Each
// glyph is two vertical bars + one horizontal cross-bar at vertical
// midpoint.
constexpr std::uint32_t k_glyph_size = 64;
constexpr std::uint32_t k_stroke_width = 4;
constexpr std::uint32_t k_grid_cols = 5;
constexpr std::uint32_t k_grid_rows = 5;

void draw_h_glyph(const PaintTarget& t, std::uint32_t cx, std::uint32_t cy) noexcept {
  const std::uint32_t half = k_glyph_size / 2U;
  if (cx < half || cy < half) {
    return;
  }
  const std::uint32_t x0 = cx - half;
  const std::uint32_t y0 = cy - half;
  const std::uint32_t x1 = cx + half;
  const std::uint32_t y1 = cy + half;
  // Left vertical stroke.
  fill_rect(t, x0, y0, x0 + k_stroke_width, y1, k_white);
  // Right vertical stroke.
  fill_rect(t, x1 - k_stroke_width, y0, x1, y1, k_white);
  // Horizontal cross-bar at vertical midpoint.
  const std::uint32_t bar_y = cy - (k_stroke_width / 2U);
  fill_rect(t, x0, bar_y, x1, bar_y + k_stroke_width, k_white);
}

void paint_h_pattern(const PaintTarget& t) noexcept {
  fill_rows(t, 0, t.height, k_black);
  // Anchor each glyph in its grid cell's centre. Skip cells whose
  // centre lies less than half a glyph from the frame edge — happens on
  // very small modes where the 5×5 grid can't fit k_glyph_size tiles.
  const std::uint32_t cell_w = t.width / k_grid_cols;
  const std::uint32_t cell_h = t.height / k_grid_rows;
  for (std::uint32_t gy = 0; gy < k_grid_rows; ++gy) {
    for (std::uint32_t gx = 0; gx < k_grid_cols; ++gx) {
      const std::uint32_t cx = (gx * cell_w) + (cell_w / 2U);
      const std::uint32_t cy = (gy * cell_h) + (cell_h / 2U);
      draw_h_glyph(t, cx, cy);
    }
  }
}

}  // namespace

void paint(PatternKind kind, const PaintTarget& t) noexcept {
  if (!valid_target(t)) {
    return;
  }
  switch (kind) {
    case PatternKind::SmpteBars:
      paint_smpte_bars(t);
      return;
    case PatternKind::PixelStripes:
      paint_pixel_stripes(t);
      return;
    case PatternKind::GrayBars:
      paint_gray_bars(t);
      return;
    case PatternKind::Checkerboard:
      paint_checkerboard(t);
      return;
    case PatternKind::GrayGradient:
      paint_gray_gradient(t);
      return;
    case PatternKind::ColorGradient:
      paint_color_gradient(t);
      return;
    case PatternKind::CrossHatch:
      paint_crosshatch(t);
      return;
    case PatternKind::HPattern:
      paint_h_pattern(t);
      return;
  }
}

const char* name_of(PatternKind kind) noexcept {
  switch (kind) {
    case PatternKind::SmpteBars:
      return "SMPTE 75% bars + PLUGE";
    case PatternKind::PixelStripes:
      return "1-pixel B/W stripes";
    case PatternKind::GrayBars:
      return "11-step gray bars";
    case PatternKind::Checkerboard:
      return "64 px checkerboard";
    case PatternKind::GrayGradient:
      return "gray gradient";
    case PatternKind::ColorGradient:
      return "R/G/B gradients";
    case PatternKind::CrossHatch:
      return "cross-hatch";
    case PatternKind::HPattern:
      return "H-pattern grid";
  }
  return "?";
}

}  // namespace drm::examples::test_patterns
