// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/negotiate.cpp

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/present/negotiate.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace drm::present {

namespace {

// Rank key: compression first, then tiling, then linear.
int class_rank(fmt::BandwidthClass c) noexcept {
  switch (c) {
    case fmt::BandwidthClass::Compression:
      return 0;
    case fmt::BandwidthClass::Tiling:
      return 1;
    case fmt::BandwidthClass::Linear:
      return 2;
  }
  return 3;
}

}  // namespace

std::vector<fmt::Modifier> negotiate(drm::span<const fmt::Modifier> producer,
                                     drm::span<const fmt::Modifier> plane, fmt::Rotation rotation) {
  std::vector<fmt::Modifier> out;
  for (const fmt::Modifier m : producer) {
    if (!fmt::rotation_compatible(m, rotation)) {
      continue;  // can't be scanned out under this rotation
    }
    const bool in_plane =
        std::any_of(plane.begin(), plane.end(), [m](fmt::Modifier p) { return p == m; });
    if (!in_plane) {
      continue;  // not in the producer-display intersection
    }
    const bool dup = std::any_of(out.begin(), out.end(), [m](fmt::Modifier e) { return e == m; });
    if (!dup) {
      out.push_back(m);
    }
  }
  // Stable so producer preference is preserved within a bandwidth class.
  std::stable_sort(out.begin(), out.end(), [](fmt::Modifier a, fmt::Modifier b) {
    return class_rank(fmt::classify(a)) < class_rank(fmt::classify(b));
  });
  return out;
}

std::vector<fmt::Modifier> negotiate(drm::span<const fmt::Modifier> producer,
                                     const fmt::FormatTable& plane, std::uint32_t fourcc,
                                     fmt::Rotation rotation) {
  return negotiate(producer, plane.modifiers_for(fourcc), rotation);
}

}  // namespace drm::present
