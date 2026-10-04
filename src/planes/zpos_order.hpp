// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Private (not installed): the stacking-order rule the allocator applies when a
// layer's zpos cannot be written to its plane.
//
// A mutable-zpos plane takes the layer's zpos (apply_layer_to_plane writes it,
// and the static gate keeps it inside the plane's range), so it stacks exactly
// where the layer asked. A fixed-slot plane (zpos_min == zpos_max — immutable,
// e.g. i.MX LCDIF / vc4 PRIMARY at 0, amdgpu PRIMARY at 2) is never written: it
// stacks at its slot whatever the layer asked for. Requiring layer.zpos == slot
// there is stricter than needed — on a single-plane controller it shuts every
// layer with zpos >= 1 out of the only plane. What actually matters is relative
// order: two placed layers must stack in the order their zpos values request.
// The kernel accepts an inverted stack happily, so a TEST commit never catches
// it; this check has to.

#pragma once

#include "plane_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

namespace drm::planes::detail {

/// True when the plane's zpos is a single fixed slot that cannot be written.
[[nodiscard]] inline bool zpos_fixed(const PlaneCapabilities& plane) noexcept {
  return plane.zpos_min.has_value() && plane.zpos_max.has_value() &&
         *plane.zpos_min == *plane.zpos_max;
}

/// Where a layer requesting `layer_zpos` actually stacks on `plane`: the fixed
/// slot, else the layer's own (written) zpos. nullopt when neither is known.
[[nodiscard]] inline std::optional<std::uint64_t> effective_zpos(
    const PlaneCapabilities& plane, std::optional<std::uint64_t> layer_zpos) noexcept {
  if (zpos_fixed(plane)) {
    return plane.zpos_min;
  }
  return layer_zpos;
}

/// True when layer A (zpos `za`) on plane `pa` and layer B (zpos `zb`) on plane
/// `pb` stack in the order their zpos values request. Layers without a zpos, or
/// with equal zpos, request no order. An equal effective slot for different
/// requested zpos is ambiguous (the kernel breaks the tie by plane id) and is
/// rejected. For every assignment the range gate admits on non-fixed planes this
/// is trivially true — effective == requested — so it only ever rules on layers
/// placed on fixed-slot planes.
[[nodiscard]] inline bool stacking_consistent(const PlaneCapabilities& pa,
                                              std::optional<std::uint64_t> za,
                                              const PlaneCapabilities& pb,
                                              std::optional<std::uint64_t> zb) noexcept {
  if (!za.has_value() || !zb.has_value() || *za == *zb) {
    return true;
  }
  const auto ea = effective_zpos(pa, za);
  const auto eb = effective_zpos(pb, zb);
  if (!ea.has_value() || !eb.has_value()) {
    return true;  // a plane without a zpos property: nothing to verify against
  }
  if (*ea == *eb) {
    return false;
  }
  return (*za < *zb) == (*ea < *eb);
}

/// One armed plane in a frame's stack: the plane, the zpos its layer requests,
/// and the zpos to write (filled by stack_zpos).
struct StackEntry {
  const PlaneCapabilities* plane{nullptr};
  std::optional<std::uint64_t> requested;
  std::optional<std::uint64_t> written;
};

/// Number the armed planes' zpos densely, in requested order, from each plane's
/// own minimum: a mutable plane gets max(previous + 1, zpos_min), a fixed-slot
/// plane keeps its slot. Only the planes actually armed take values, so layers
/// that end up composited don't use up the range, and the stack stays clear of
/// the top of it, which some controllers advertise but reject. Order is
/// preserved, so stacking_consistent() rulings stand. Greedy-lowest is optimal:
/// when it overflows a plane's zpos_max (or meets a fixed slot at or below the
/// previous value) no dense numbering exists; then `written` is left equal to
/// `requested` and false is returned. Entries without a requested zpos or a
/// zpos range are not numbered.
[[nodiscard]] inline bool stack_zpos(std::vector<StackEntry>& entries) {
  struct Rankable {
    std::size_t index;
    std::uint64_t requested;
    std::uint32_t plane_id;
    std::uint64_t zmin;
    std::uint64_t zmax;
    bool fixed;
  };
  std::vector<Rankable> order;
  order.reserve(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    auto& e = entries[i];
    e.written = e.requested;
    if (e.plane == nullptr) {
      continue;
    }
    const auto req = e.requested;
    const auto zmin = e.plane->zpos_min;
    const auto zmax = e.plane->zpos_max;
    if (req.has_value() && zmin.has_value() && zmax.has_value()) {
      order.push_back({i, *req, e.plane->id, *zmin, *zmax, *zmin == *zmax});
    }
  }
  std::stable_sort(order.begin(), order.end(), [](const Rankable& a, const Rankable& b) {
    return std::tie(a.requested, a.plane_id) < std::tie(b.requested, b.plane_id);
  });
  std::vector<std::uint64_t> values(order.size());
  std::optional<std::uint64_t> prev;
  for (std::size_t k = 0; k < order.size(); ++k) {
    const auto& r = order[k];
    std::uint64_t v = r.zmin;
    if (prev.has_value()) {
      if (r.fixed && v <= *prev) {
        return false;
      }
      if (!r.fixed) {
        v = std::max(*prev + 1, r.zmin);
      }
    }
    if (v > r.zmax) {
      return false;
    }
    values[k] = v;
    prev = v;
  }
  for (std::size_t k = 0; k < order.size(); ++k) {
    entries[order[k].index].written = values[k];
  }
  return true;
}

}  // namespace drm::planes::detail
