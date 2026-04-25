// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// overlay_renderer — paints the signage_player overlay band into a
// CPU-mapped ARGB8888 buffer. Background fill is straight-alpha →
// premultiplied at the boundary so user-supplied "#RRGGBBAA" values
// match KMS's premultiplied scanout convention. Text is drawn with
// Blend2D when the umbrella header is reachable; otherwise the
// function degrades gracefully to bg-only.

#pragma once

#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <string_view>

namespace signage {

/// All colours are straight-alpha 0xAARRGGBB (the form `parse_color`
/// produces). The renderer premultiplies before pixel write.
struct OverlayPaint {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t stride_bytes{};
  std::uint32_t fg_argb{0xFFFFFFFFU};
  std::uint32_t bg_argb{0x80000000U};
  std::uint32_t font_size{32};
  std::string_view text;
};

/// Paint `p.bg_argb` over the entire buffer, then centre `p.text` on
/// top in `p.fg_argb`. No-op for the text portion when Blend2D is
/// unavailable, the text is empty, or no system font could be located.
void paint_overlay(drm::span<std::uint8_t> pixels, const OverlayPaint& p) noexcept;

/// Single-frame paint of a horizontally-scrolling marquee. The scroll
/// position is supplied by the caller so this function stays stateless;
/// each call repaints the whole buffer (background + repeated text
/// copies). Designed to be called every frame — the dirty-every-frame
/// workload is what makes this layer useful as a Phase 2.2 testbed.
struct TickerPaint {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t stride_bytes{};
  std::uint32_t fg_argb{0xFFFFFFFFU};
  std::uint32_t bg_argb{0xC0000000U};
  std::uint32_t font_size{24};
  /// Monotonically advancing pixel offset (caller multiplies elapsed
  /// seconds by pixels_per_second). The renderer modulos against the
  /// per-pass text width internally so callers don't need to.
  double scroll_offset_px{0.0};
  std::string_view text;
};

void paint_ticker(drm::span<std::uint8_t> pixels, const TickerPaint& p) noexcept;

/// Single-frame paint of a small clock badge. The current time string
/// is supplied by the caller so this function stays stateless and
/// dependency-free; the demo only invokes it when the formatted string
/// changes (once per minute with the default "%H:%M"), which is what
/// makes this layer useful as the dirty-once-per-minute counterpart to
/// the static overlay and dirty-every-frame ticker.
struct ClockPaint {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t stride_bytes{};
  std::uint32_t fg_argb{0xFFFFFFFFU};
  std::uint32_t bg_argb{0x80000000U};
  std::uint32_t font_size{48};
  std::string_view text;
};

void paint_clock(drm::span<std::uint8_t> pixels, const ClockPaint& p) noexcept;

}  // namespace signage