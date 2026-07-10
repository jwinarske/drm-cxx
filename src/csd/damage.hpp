// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/damage.hpp — per-decoration damage tracking shared by the
// software-compositing presenters (composite onto a KMS plane, and blit
// onto /dev/fb0).
//
// Both presenters draw N decorations into a persistent buffer and want to
// re-touch only what changed between frames. They snapshot each slot as a
// `DamageSlot` (armed? where? which content generation?) and diff this
// frame's against last frame's to get a bounding `DamageRect`: the region
// that opened, closed, moved, resized, or repainted. The presenter then
// clears + re-blends only that region (and, for the fb path, converts only
// its scanlines). The content-change signal is `Surface::content_generation()`.

#pragma once

#include <drm-cxx/detail/span.hpp>

#include <cstdint>

namespace drm::csd {

/// One decoration slot's snapshot, diffed frame-to-frame to find damage.
/// `gen` is the surface's `content_generation()` at snapshot time.
struct DamageSlot {
  bool armed{false};
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint32_t w{0};
  std::uint32_t h{0};
  std::uint64_t gen{0};
};

/// A canvas-space damage rectangle (half-open extents).
struct DamageRect {
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint32_t w{0};
  std::uint32_t h{0};
  [[nodiscard]] bool empty() const noexcept { return w == 0U || h == 0U; }
};

/// The bounding damage rectangle for the change from `prev` to `cur`
/// (index-matched slots), clamped to `canvas_w x canvas_h`. A slot
/// contributes its old and/or new rect when it opened, closed, moved,
/// resized, or its content generation changed. A slot-count change (or an
/// empty `prev`, i.e. the first frame) damages the whole canvas.
[[nodiscard]] DamageRect compute_damage(drm::span<const DamageSlot> prev,
                                        drm::span<const DamageSlot> cur, std::uint32_t canvas_w,
                                        std::uint32_t canvas_h);

/// Intersection of the rect `(x,y,w,h)` with `r`; empty if disjoint.
[[nodiscard]] DamageRect intersect_rect(std::int32_t x, std::int32_t y, std::uint32_t w,
                                        std::uint32_t h, const DamageRect& r);

/// Bounding union of two rects (either may be empty).
[[nodiscard]] DamageRect union_rect(const DamageRect& a, const DamageRect& b);

}  // namespace drm::csd
