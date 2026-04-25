// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// patterns.hpp — display-engineering reference patterns rendered into a
// CPU-mapped XRGB8888 buffer. Pure CPU paint; no DRM ioctls, no
// allocations after the buffer is mapped. The buffer is the live
// scanout surface (DumbBufferSource hands out the same fb_id every
// acquire) so the painter overwrites in place — tearing on the switch
// is harmless for static patterns.

#pragma once

#include <drm-cxx/detail/span.hpp>

#include <cstdint>

namespace drm::examples::test_patterns {

/// Selectable pattern kinds. The numeric values double as keyboard
/// shortcuts ('1' through '8') in main.cpp's input handler.
enum class PatternKind : std::uint8_t {
  SmpteBars,      // '1' — SMPTE 75% bars + PLUGE-style sub-bands
  PixelStripes,   // '2' — 1-pixel B/W vertical alternation
  GrayBars,       // '3' — 11 discrete gray-step bars
  Checkerboard,   // '4' — 64 px black/white tiles
  GrayGradient,   // '5' — horizontal 0→255 gray sweep
  ColorGradient,  // '6' — three stacked R/G/B horizontal sweeps
  CrossHatch,     // '7' — 64 px white grid on black
  HPattern,       // '8' — procedural H glyph grid (focus uniformity)
};

/// Number of distinct PatternKind values; useful for iteration and
/// next/previous-with-wrap arithmetic in the input handler.
inline constexpr std::uint8_t k_pattern_count = 8;

struct PaintTarget {
  /// CPU mapping over the buffer's linear pixel storage. Must hold at
  /// least `height * stride_bytes` bytes; the painter validates and
  /// returns silently on a misshapen target.
  drm::span<std::uint8_t> pixels{};
  /// Bytes per row. May exceed `width * 4` when the kernel pads scanlines.
  std::uint32_t stride_bytes{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
};

/// Paint `kind` into `tgt`. Pixel format is XRGB8888 (`0x00RRGGBB`),
/// matching `DRM_FORMAT_XRGB8888` scanout. No-op on degenerate dims or
/// if `tgt.pixels` is too small for the claimed geometry.
void paint(PatternKind kind, const PaintTarget& tgt) noexcept;

/// Short, stable name for each pattern (used in the README's key-binding
/// table and printed on stdout each time the pattern changes).
[[nodiscard]] const char* name_of(PatternKind kind) noexcept;

}  // namespace drm::examples::test_patterns
