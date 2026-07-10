// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "damage.hpp"

#include <drm-cxx/detail/span.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace drm::csd {

namespace {

// Signed bounding-box accumulator for damage rects.
struct Bounds {
  bool valid{false};
  std::int32_t x0{0};
  std::int32_t y0{0};
  std::int32_t x1{0};
  std::int32_t y1{0};
};

void add_rect(Bounds& b, std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) {
  if (w == 0U || h == 0U) {
    return;
  }
  const std::int32_t rx1 = x + static_cast<std::int32_t>(w);
  const std::int32_t ry1 = y + static_cast<std::int32_t>(h);
  if (!b.valid) {
    b = {true, x, y, rx1, ry1};
    return;
  }
  b.x0 = std::min(b.x0, x);
  b.y0 = std::min(b.y0, y);
  b.x1 = std::max(b.x1, rx1);
  b.y1 = std::max(b.y1, ry1);
}

}  // namespace

DamageRect compute_damage(drm::span<const DamageSlot> prev, drm::span<const DamageSlot> cur,
                          std::uint32_t canvas_w, std::uint32_t canvas_h) {
  const DamageRect whole{0, 0, canvas_w, canvas_h};
  // First frame (no prior state) or a slot-count change: repaint everything
  // — the backdrop has to be laid down and the diff below is index-matched.
  if (prev.size() != cur.size() || prev.empty()) {
    return whole;
  }

  Bounds b;
  for (std::size_t i = 0; i < cur.size(); ++i) {
    const auto& p = prev[i];
    const auto& c = cur[i];
    const bool changed =
        (p.armed != c.armed) ||
        (c.armed && (p.x != c.x || p.y != c.y || p.w != c.w || p.h != c.h || p.gen != c.gen));
    if (!changed) {
      continue;
    }
    if (p.armed) {
      add_rect(b, p.x, p.y, p.w, p.h);  // vacate the old footprint
    }
    if (c.armed) {
      add_rect(b, c.x, c.y, c.w, c.h);  // paint the new one
    }
  }
  if (!b.valid) {
    return {};  // nothing changed this frame
  }

  // Clamp the bounding box to the canvas.
  const std::int32_t x0 = std::max(0, b.x0);
  const std::int32_t y0 = std::max(0, b.y0);
  const std::int32_t x1 = std::min(static_cast<std::int32_t>(canvas_w), b.x1);
  const std::int32_t y1 = std::min(static_cast<std::int32_t>(canvas_h), b.y1);
  if (x1 <= x0 || y1 <= y0) {
    return {};
  }
  return {x0, y0, static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)};
}

DamageRect intersect_rect(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h,
                          const DamageRect& r) {
  const std::int32_t x0 = std::max(x, r.x);
  const std::int32_t y0 = std::max(y, r.y);
  const std::int32_t x1 =
      std::min(x + static_cast<std::int32_t>(w), r.x + static_cast<std::int32_t>(r.w));
  const std::int32_t y1 =
      std::min(y + static_cast<std::int32_t>(h), r.y + static_cast<std::int32_t>(r.h));
  if (x1 <= x0 || y1 <= y0) {
    return {};
  }
  return {x0, y0, static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)};
}

DamageRect union_rect(const DamageRect& a, const DamageRect& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  const std::int32_t x0 = std::min(a.x, b.x);
  const std::int32_t y0 = std::min(a.y, b.y);
  const std::int32_t x1 =
      std::max(a.x + static_cast<std::int32_t>(a.w), b.x + static_cast<std::int32_t>(b.w));
  const std::int32_t y1 =
      std::max(a.y + static_cast<std::int32_t>(a.h), b.y + static_cast<std::int32_t>(b.h));
  return {x0, y0, static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)};
}

}  // namespace drm::csd
