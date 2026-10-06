// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Private (not installed): the pixel format a cursor plane is driven in.
//
// A cursor needs alpha, and the renderer holds ARGB8888. Some cursor planes
// carry the same eight-bit channels in another order (Tegra: RGBA8888 only),
// so the renderer takes the first of ARGB8888, RGBA8888, ABGR8888, BGRA8888
// the plane supports and reorders each pixel on the way into the buffer. No
// opaque format (XRGB8888 and friends) is ever accepted: the cursor would
// become a rectangle. The legacy drmModeSetCursor path stays ARGB8888; it has
// no format to negotiate.

#pragma once

#include "../planes/plane_registry.hpp"

#include <drm_fourcc.h>

#include <array>
#include <cstdint>
#include <optional>

namespace drm::cursor::detail {

/// Formats a cursor plane may be driven in, best first.
inline constexpr std::array<std::uint32_t, 4> k_cursor_formats{
    DRM_FORMAT_ARGB8888, DRM_FORMAT_RGBA8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_BGRA8888};

/// The format to drive `plane` in, or nullopt when it carries none of them.
[[nodiscard]] inline std::optional<std::uint32_t> cursor_format(
    const drm::planes::PlaneCapabilities& plane) {
  for (const auto fmt : k_cursor_formats) {
    if (plane.supports_format(fmt)) {
      return fmt;
    }
  }
  return std::nullopt;
}

/// `argb` (an ARGB8888 word) as the same pixel in `fourcc`, one of
/// k_cursor_formats.
[[nodiscard]] constexpr std::uint32_t from_argb(std::uint32_t argb, std::uint32_t fourcc) noexcept {
  const std::uint32_t a = argb >> 24U;
  const std::uint32_t r = (argb >> 16U) & 0xFFU;
  const std::uint32_t g = (argb >> 8U) & 0xFFU;
  const std::uint32_t b = argb & 0xFFU;
  switch (fourcc) {
    case DRM_FORMAT_RGBA8888:
      return (r << 24U) | (g << 16U) | (b << 8U) | a;
    case DRM_FORMAT_ABGR8888:
      return (a << 24U) | (b << 16U) | (g << 8U) | r;
    case DRM_FORMAT_BGRA8888:
      return (b << 24U) | (g << 16U) | (r << 8U) | a;
    default:
      return argb;
  }
}

}  // namespace drm::cursor::detail
