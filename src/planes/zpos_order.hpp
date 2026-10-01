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

#include <cstdint>
#include <optional>

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

}  // namespace drm::planes::detail
