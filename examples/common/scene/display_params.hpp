// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// display_params.hpp — how the scene displays a layer's buffer.
//
// Separated from LayerBufferSource's SourceFormat per the KMS concept
// boundary: the buffer source describes what the buffer IS (format,
// modifier, intrinsic size), while DisplayParams describes how the
// plane that scans it out should be configured (src/dst rect, rotation,
// alpha, zpos). Same buffer can be displayed multiple ways; same
// plane configuration can scan different buffers.

#pragma once

#include <drm-cxx/planes/layer.hpp>

#include <cstdint>

namespace drm::scene {

/// Reuse the geometry primitive already defined by drm::planes::Layer
/// so scene → planes::Layer lowering is a trivial field copy rather
/// than a type conversion. int32_t x/y match KMS CRTC_X/Y (signed —
/// planes can extend off-screen); uint32_t w/h match CRTC_W/H.
using Rect = drm::planes::Rect;

/// Per-layer display configuration. Lowered to plane properties at
/// commit time: src_rect → SRC_X/Y/W/H (scaled by 16 for the kernel's
/// 16.16 fixed-point convention); dst_rect → CRTC_X/Y/W/H; rotation →
/// the plane's rotation property (or software pre-rotation if the
/// plane lacks the property); alpha → plane.alpha property when
/// present; zpos → plane.zpos.
///
/// needs_scaling is derived, not stored: src_rect.{w,h} != dst_rect.{w,h}
/// implies the plane must support scaling.
struct DisplayParams {
  Rect src_rect{};
  Rect dst_rect{};
  std::uint64_t rotation{0};        // DRM_MODE_ROTATE_* | DRM_MODE_REFLECT_*
  std::uint16_t alpha{0xFFFF};      // 0xFFFF = fully opaque
  int zpos{0};

  [[nodiscard]] constexpr bool needs_scaling() const noexcept {
    return src_rect.w != dst_rect.w || src_rect.h != dst_rect.h;
  }
};

}  // namespace drm::scene
