// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// composite_canvas.hpp — software composition target for layers the
// allocator could not place on a hardware plane.
//
// Phase 2.1 ships only the skeleton: the config struct plus a forward
// declaration for the class. The real CompositeCanvas is Phase 2.3's
// centerpiece — a pool of ARGB8888 dumb buffers at scene resolution,
// with damage-tracked blend() operations, that feeds unassigned layers
// back into a second-pass allocator as a synthetic merged layer.
//
// LayerScene refers to CompositeCanvas via pointer / reference only in
// Phase 2.1, so this forward declaration is sufficient to build the
// scene until 2.3 lands the implementation.

#pragma once

#include <cstdint>

namespace drm::scene {

struct CompositeCanvasConfig {
  /// How many ARGB8888 dumb buffers the canvas pool may allocate.
  /// Composition can juggle up to `max_canvases` independent buckets
  /// before it has to serialize behind a shared canvas.
  std::uint32_t max_canvases{4};

  /// Dimensions of each canvas. Normally the scene's CRTC mode size;
  /// exposed here so headless / offscreen scenes can size them
  /// explicitly.
  std::uint32_t canvas_width{0};
  std::uint32_t canvas_height{0};
};

class CompositeCanvas;  // defined in Phase 2.3

}  // namespace drm::scene
