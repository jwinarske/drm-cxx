// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// status_overlay_renderer — paints the camera example's bottom-right
// "fps=N cameras=M" badge into a CPU-mapped ARGB8888 buffer.
//
// Modeled on examples/scene/signage_player/overlay_renderer; the badge
// is a small fixed-size rectangle (caller sizes it, typically 320x56)
// with a translucent background and centered text. Built only when
// the umbrella project gates Blend2D on; the gate is plumbed via the
// build system as CAMERA_STATUS_HAS_BLEND2D so a stray header probe
// from a tidy run without -isystem flags doesn't pull BL paths into
// the TU.

#pragma once

#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <string_view>

namespace camera {

struct StatusPaint {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t stride_bytes{};
  std::uint32_t fg_argb{0xFFFFFFFFU};
  std::uint32_t bg_argb{0xC0000000U};
  std::uint32_t font_size{20};
  std::string_view text;
};

void paint_status(drm::span<std::uint8_t> pixels, const StatusPaint& p) noexcept;

}  // namespace camera