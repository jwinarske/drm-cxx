// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::planes {

enum class DRMPlaneType : uint8_t {
  PRIMARY,
  OVERLAY,
  CURSOR,
};

struct PlaneCapabilities {
  uint32_t id{};
  uint32_t possible_crtcs{};
  DRMPlaneType type{DRMPlaneType::OVERLAY};
  std::vector<uint32_t> formats;
  /// (format, modifier) pairs harvested from the IN_FORMATS blob, sorted
  /// by format ascending so `supports_format_modifier()` can locate the
  /// per-format slice in O(log N). Empty when the driver doesn't expose
  /// IN_FORMATS at all (older legacy stacks); in that case
  /// `has_format_modifiers` is false and `supports_format_modifier()`
  /// falls back to format-only matching for the LINEAR / INVALID
  /// modifiers. Non-trivial modifiers (AFBC, DCC, vendor tilings) are
  /// rejected when this is empty.
  std::vector<std::pair<uint32_t, uint64_t>> format_modifiers;
  std::optional<uint64_t> zpos_min;
  std::optional<uint64_t> zpos_max;
  bool supports_rotation{false};
  bool supports_scaling{false};
  bool has_format_modifiers{false};
  uint32_t cursor_max_w{};
  uint32_t cursor_max_h{};

  [[nodiscard]] bool supports_format(uint32_t fmt) const;

  /// Check `(format, modifier)` against IN_FORMATS data when present, or
  /// against the bare format list when absent. LINEAR and INVALID are
  /// treated as equivalent (INVALID is the legacy sentinel many drivers
  /// still emit for "use whatever — typically linear").
  [[nodiscard]] bool supports_format_modifier(uint32_t fmt, uint64_t modifier) const;

  [[nodiscard]] bool compatible_with_crtc(uint32_t crtc_index) const;
};

class PlaneRegistry {
 public:
  static drm::expected<PlaneRegistry, std::error_code> enumerate(const Device& dev);

  // Copying would invalidate the for_crtc cache (its pointers would
  // dangle into the source's `planes_`), and the enumerate-then-move
  // construction pattern is the only consumer; explicit delete keeps
  // accidental copies from compiling. Move preserves vector buffer
  // pointers, so the cache survives a move intact.
  PlaneRegistry() = default;
  PlaneRegistry(const PlaneRegistry&) = delete;
  PlaneRegistry& operator=(const PlaneRegistry&) = delete;
  PlaneRegistry(PlaneRegistry&&) noexcept = default;
  PlaneRegistry& operator=(PlaneRegistry&&) noexcept = default;
  ~PlaneRegistry() = default;

  [[nodiscard]] drm::span<const PlaneCapabilities> all() const noexcept;

  /// Planes compatible with `crtc_index`. The returned reference is
  /// stable for the lifetime of the registry — `for_crtc` lazily
  /// populates a per-CRTC cache on first call so repeated lookups for
  /// the same CRTC are O(1) and allocation-free.
  [[nodiscard]] const std::vector<const PlaneCapabilities*>& for_crtc(uint32_t crtc_index) const;

 private:
  std::vector<PlaneCapabilities> planes_;
  // Lazy per-CRTC view; mutable so const lookups can populate it.
  // Pointers reference elements in `planes_`, whose addresses are
  // stable for the registry's lifetime (vector is built once in
  // enumerate() and never reallocated thereafter).
  mutable std::unordered_map<uint32_t, std::vector<const PlaneCapabilities*>> for_crtc_cache_;
};

}  // namespace drm::planes
