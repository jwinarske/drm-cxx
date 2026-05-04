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

/// Plane `COLOR_ENCODING` enum. Selects the YCbCr → RGB matrix the
/// display engine applies when scanning out a YUV format. Ignored by
/// the kernel for RGB formats. Writing a value that the plane doesn't
/// expose (because the property's enum table omits it) is silently
/// skipped at commit time — `PlaneCapabilities` caches the integer
/// each driver assigns to each entry, so callers can detect unsupported
/// values up front via the `color_encoding_*` optionals.
enum class ColorEncoding : uint8_t {
  BT_601,
  BT_709,
  BT_2020,
};

/// Plane `COLOR_RANGE` enum. Selects whether YCbCr 8-bit data is
/// interpreted as full-range (0..255) or limited-range (16..235 for
/// luma, 16..240 for chroma). Most camera and broadcast YCbCr is
/// limited-range; PC-generated YCbCr is sometimes full-range.
enum class ColorRange : uint8_t {
  Limited,
  Full,
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
  /// True when the plane exposes the `"pixel blend mode"` enum property.
  /// Absence means the plane has no blend control and scanout is
  /// hardwired by the driver — typically opaque pixel-replace, so a
  /// transparent (alpha=0) source pixel paints opaque black over
  /// whatever lies beneath in the plane stack.
  bool has_pixel_blend_mode{false};
  /// True when the plane exposes the `"alpha"` u16 per-plane alpha
  /// property. Independent of `has_pixel_blend_mode` — some hardware
  /// exposes one without the other.
  bool has_per_plane_alpha{false};
  /// Cached enum integer for `"pixel blend mode" = "Pre-multiplied"`,
  /// when the plane advertises that enum value. The integer is
  /// driver-defined — the kernel hands back enum values in the order
  /// the driver registered them, so callers must look up by name.
  /// Use this to write the property via an atomic commit. nullopt
  /// when the property doesn't expose the value at all (rare).
  std::optional<uint64_t> blend_mode_premultiplied;
  /// As `blend_mode_premultiplied`, for `"Coverage"` (straight-alpha
  /// SRC_OVER). Most drivers expose both Coverage and Pre-multiplied;
  /// callers should prefer Pre-multiplied to match the canvas's
  /// internal premultiplied output convention.
  std::optional<uint64_t> blend_mode_coverage;
  /// As `blend_mode_premultiplied`, for `"None"` (pixel-replace, no
  /// blend). Surfaced for diagnostic / explicit-replace use cases.
  std::optional<uint64_t> blend_mode_none;
  /// True when the plane exposes the `"COLOR_ENCODING"` enum property.
  /// Only YCbCr-capable planes set this — RGB-only planes leave it
  /// false and `LayerScene` skips writing the property for them.
  bool has_color_encoding{false};
  /// Cached enum integer for each `COLOR_ENCODING` entry. nullopt when
  /// the plane lacks that specific entry (e.g. older drivers expose
  /// only BT.601 + BT.709, no BT.2020).
  std::optional<uint64_t> color_encoding_bt601;
  std::optional<uint64_t> color_encoding_bt709;
  std::optional<uint64_t> color_encoding_bt2020;
  /// True when the plane exposes the `"COLOR_RANGE"` enum property.
  bool has_color_range{false};
  /// Cached enum integer for each `COLOR_RANGE` entry. nullopt when
  /// the plane lacks that specific entry.
  std::optional<uint64_t> color_range_limited;
  std::optional<uint64_t> color_range_full;
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

  // Build a registry directly from a caller-supplied capability list.
  // Used by synthetic-source consumers (notably unit tests for helpers
  // that take a `const PlaneRegistry&`) and by replay/snapshot tools
  // that have a serialized capability set rather than a live device.
  // The resulting registry behaves identically to one returned by
  // `enumerate` — `all()` and `for_crtc()` see the supplied entries.
  static PlaneRegistry from_capabilities(std::vector<PlaneCapabilities> caps);

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
