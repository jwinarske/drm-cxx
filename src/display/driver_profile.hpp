// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// display/driver_profile.hpp
//
// Device-level capabilities, probed from drmGetCap + drmGetVersion. Consolidates
// the "what can this DRM node do" queries a presenter needs before it commits.
// Capability-driven by design: `name` is informational only and must not be
// branched on -- gate behavior on the capability flags, which is how the rest of
// the library already handles driver differences. Per-connector and per-CRTC
// capabilities live in connector_capabilities / crtc_capabilities; per-plane in
// the plane registry.

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <string>
#include <system_error>

namespace drm {
class Device;
}

namespace drm::display {

struct DriverProfile {
  std::string name;              // drmGetVersion()->name; informational, never branched on
  bool addfb2_modifiers{false};  // DRM_CAP_ADDFB2_MODIFIERS: AddFB2 accepts explicit modifiers
  bool async_page_flip{false};   // DRM_CAP_ASYNC_PAGE_FLIP
  bool timestamp_monotonic{
      false};                      // DRM_CAP_TIMESTAMP_MONOTONIC: vblank stamps are CLOCK_MONOTONIC
  bool prime_import{false};        // DRM_CAP_PRIME & DRM_PRIME_CAP_IMPORT
  bool prime_export{false};        // DRM_CAP_PRIME & DRM_PRIME_CAP_EXPORT
  std::uint64_t cursor_width{64};  // DRM_CAP_CURSOR_WIDTH  (64 when the driver reports nothing)
  std::uint64_t cursor_height{64};  // DRM_CAP_CURSOR_HEIGHT (64 when the driver reports nothing)

  [[nodiscard]] static drm::expected<DriverProfile, std::error_code> probe(const drm::Device& dev);
};

// Decoded DRM_CAP_PRIME bitmask. Pure helper, exposed for testing.
struct PrimeCaps {
  bool can_import{false};
  bool can_export{false};
};
[[nodiscard]] PrimeCaps decode_prime_caps(std::uint64_t cap) noexcept;

}  // namespace drm::display
