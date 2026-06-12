// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// draw.hpp — tiny format-aware CPU drawing helpers for the dumb-buffer demos.
//
// The small present examples draw flat rects into a mapped dumb buffer. They
// used to hardcode 32-bpp XRGB8888 writes, which breaks on display controllers
// that only scan out other packed formats (e.g. TI tilcdc: RGB565 only). These
// helpers take the scanout fourcc (from present::negotiate_scanout_format) and
// pack an 0xAARRGGBB color into XRGB8888 (32 bpp) or RGB565 (16 bpp) accordingly.

#pragma once

#include <drm-cxx/buffer_mapping.hpp>

#include <drm_fourcc.h>

#include <cstdint>
#include <cstring>

namespace drm::examples {

/// Bytes per pixel for the packed formats these demos render (2 for RGB565,
/// else 4 for XRGB/ARGB8888).
[[nodiscard]] inline std::uint32_t bytes_per_pixel(std::uint32_t fourcc) {
  return fourcc == DRM_FORMAT_RGB565 ? 2U : 4U;
}

/// Pack 0xAARRGGBB into the scanout format's little-endian pixel value (low
/// bytes used by fill_rect's memcpy).
[[nodiscard]] inline std::uint32_t pack_argb(std::uint32_t fourcc, std::uint32_t argb) {
  if (fourcc == DRM_FORMAT_RGB565) {
    const std::uint32_t r = (argb >> 16) & 0xFFU;
    const std::uint32_t g = (argb >> 8) & 0xFFU;
    const std::uint32_t b = argb & 0xFFU;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  }
  return argb;  // XRGB/ARGB8888: stored as-is
}

/// Fill a w×h rect at (x,y) with `argb`, packed for `fourcc`. Clipped to the
/// mapping bounds.
inline void fill_rect(drm::BufferMapping& m, std::uint32_t fourcc, std::int32_t x, std::int32_t y,
                      std::int32_t w, std::int32_t h, std::uint32_t argb) {
  const auto pixels = m.pixels();
  if (pixels.data() == nullptr) {
    return;
  }
  const auto stride = static_cast<std::int32_t>(m.stride());
  const auto bpp = static_cast<std::int32_t>(bytes_per_pixel(fourcc));
  const auto max_x = static_cast<std::int32_t>(m.width());
  const auto max_y = static_cast<std::int32_t>(m.height());
  const std::uint32_t value = pack_argb(fourcc, argb);
  for (std::int32_t row = (y < 0 ? 0 : y); row < y + h && row < max_y; ++row) {
    std::uint8_t* line = pixels.data() + (static_cast<std::size_t>(row) * stride);
    for (std::int32_t col = (x < 0 ? 0 : x); col < x + w && col < max_x; ++col) {
      std::memcpy(line + (static_cast<std::size_t>(col) * bpp), &value,
                  static_cast<std::size_t>(bpp));
    }
  }
}

/// Clear the whole mapping to `argb`, packed for `fourcc`.
inline void clear(drm::BufferMapping& m, std::uint32_t fourcc, std::uint32_t argb) {
  fill_rect(m, fourcc, 0, 0, static_cast<std::int32_t>(m.width()),
            static_cast<std::int32_t>(m.height()), argb);
}

}  // namespace drm::examples
