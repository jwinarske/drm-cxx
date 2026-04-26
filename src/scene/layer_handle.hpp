// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// layer_handle.hpp — opaque stable identity for a scene layer.
//
// Consumers hold a LayerHandle and pass it back to LayerScene for
// get_layer / remove_layer / mutate operations. The handle survives
// across frames; the scene's freelist recycles slot ids with a
// monotonic generation counter, so a handle to a removed layer never
// accidentally addresses a newly-added one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace drm::scene {

/// Opaque, generation-tagged identity for a scene layer. Returned by
/// `LayerScene::add_layer`; passed to `get_layer` / `remove_layer`
/// and the layer's `set_*` methods. Survives `rebind()` and
/// `on_session_resumed()`. A handle whose layer was removed and whose
/// slot has been recycled never accidentally addresses the new
/// occupant — `get_layer` returns nullptr because the slot's current
/// generation no longer matches.
struct LayerHandle {
  /// 1-based slot index into the scene's layer table. 0 == invalid /
  /// default-constructed.
  std::uint32_t id{0};
  /// Incremented each time the slot is re-used. Catches use-after-
  /// remove: the handle's generation won't match the slot's current
  /// generation, so get_layer returns nullptr.
  std::uint32_t generation{0};

  /// True if this handle is not the default-constructed sentinel. Does
  /// not check against the scene — a `valid()` handle can still be
  /// stale (its layer was removed). Use `LayerScene::get_layer` to
  /// resolve liveness.
  [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }

  friend constexpr bool operator==(LayerHandle lhs, LayerHandle rhs) noexcept {
    return lhs.id == rhs.id && lhs.generation == rhs.generation;
  }
  friend constexpr bool operator!=(LayerHandle lhs, LayerHandle rhs) noexcept {
    return !(lhs == rhs);
  }
};

}  // namespace drm::scene

template <>
struct std::hash<drm::scene::LayerHandle> {
  std::size_t operator()(drm::scene::LayerHandle h) const noexcept {
    // Splay id into the upper 32 bits, generation into the lower — the
    // two fields are independent enough that XOR-mixing would collide
    // every time id == generation (both start at 1 for slot 0).
    return (static_cast<std::size_t>(h.id) << 32U) ^ h.generation;
  }
};
