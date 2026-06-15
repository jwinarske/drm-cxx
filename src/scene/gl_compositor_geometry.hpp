// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// gl_compositor_geometry.hpp — pure quad/texcoord math for GlCompositor,
// split out so it is unit-testable without an EGL/GLES context (and
// without the DRM_CXX_HAS_EGL gate). No GL dependency.

#pragma once

#include <array>
#include <cstdint>

namespace drm::scene::gl_geom {

/// One textured-quad vertex: position in clip-space NDC, texcoord in [0,1].
struct QuadVertex {
  float x{0.0F};
  float y{0.0F};
  float u{0.0F};
  float v{0.0F};
};

/// Build the 4 vertices (GL_TRIANGLE_STRIP order TL, BL, TR, BR) for a layer
/// blit: destination rect `dst` (pixels, top-left origin) on a `canvas_w x
/// canvas_h` target, sampling source rect `src` (pixels) from a `src_w x src_h`
/// texture.
///
/// NDC y is flipped (`1 - 2*py/H`): GL renders with a bottom-left origin into
/// the gbm buffer, but the scanout engine reads it top-left, so the flip makes
/// the canvas's top-left land at the display's top-left — matching the CPU
/// CompositeCanvas. Texcoord v is NOT flipped: the source texture is uploaded
/// row 0 = top, so v=0 is the source top.
[[nodiscard]] inline std::array<QuadVertex, 4> quad(std::int32_t dst_x, std::int32_t dst_y,
                                                    std::uint32_t dst_w, std::uint32_t dst_h,
                                                    std::uint32_t canvas_w, std::uint32_t canvas_h,
                                                    std::int32_t src_x, std::int32_t src_y,
                                                    std::uint32_t src_rw, std::uint32_t src_rh,
                                                    std::uint32_t src_w, std::uint32_t src_h) {
  const float cw = (canvas_w != 0U) ? static_cast<float>(canvas_w) : 1.0F;
  const float ch = (canvas_h != 0U) ? static_cast<float>(canvas_h) : 1.0F;
  const float tw = (src_w != 0U) ? static_cast<float>(src_w) : 1.0F;
  const float th = (src_h != 0U) ? static_cast<float>(src_h) : 1.0F;

  const float x0 = (2.0F * static_cast<float>(dst_x) / cw) - 1.0F;
  const float x1 =
      (2.0F * static_cast<float>(dst_x + static_cast<std::int32_t>(dst_w)) / cw) - 1.0F;
  const float y0 = 1.0F - (2.0F * static_cast<float>(dst_y) / ch);
  const float y1 =
      1.0F - (2.0F * static_cast<float>(dst_y + static_cast<std::int32_t>(dst_h)) / ch);

  const float u0 = static_cast<float>(src_x) / tw;
  const float u1 = static_cast<float>(src_x + static_cast<std::int32_t>(src_rw)) / tw;
  const float v0 = static_cast<float>(src_y) / th;
  const float v1 = static_cast<float>(src_y + static_cast<std::int32_t>(src_rh)) / th;

  return std::array<QuadVertex, 4>{{
      {x0, y0, u0, v0},  // top-left
      {x0, y1, u0, v1},  // bottom-left
      {x1, y0, u1, v0},  // top-right
      {x1, y1, u1, v1},  // bottom-right
  }};
}

}  // namespace drm::scene::gl_geom
