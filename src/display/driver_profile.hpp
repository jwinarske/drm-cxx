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

// Best-effort panel self-refresh classification — telemetry only, never gates
// behavior (frame_economy's idle-Skip is unconditional). PSR has no portable KMS
// property; the portable necessary signal is the connector type, so this is the
// first layer of the migration plan's PSR detection (a connected eDP/DSI panel
// *may* self-refresh; anything external can't).
enum class PanelSelfRefresh : std::uint8_t {
  Unknown,   // no connected connector, or the connector set couldn't be read
  None,      // every connected connector is external (HDMI/DP/...) — cannot self-refresh
  Possible,  // a connected eDP / DSI panel — may support PSR (not authoritative)
};

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

  // Frame-economy capabilities (per-object, probed by enumeration — not drmGetCap).
  bool fb_damage_clips{false};  // >=1 plane advertises FB_DAMAGE_CLIPS (driver consumes damage)
  bool vrr_capable{false};      // >=1 CRTC exposes VRR_ENABLED (variable refresh is drivable)
  PanelSelfRefresh psr{PanelSelfRefresh::Unknown};  // telemetry only; see frame_economy

  [[nodiscard]] static drm::expected<DriverProfile, std::error_code> probe(const drm::Device& dev);
};

// Decoded DRM_CAP_PRIME bitmask. Pure helper, exposed for testing.
struct PrimeCaps {
  bool can_import{false};
  bool can_export{false};
};
[[nodiscard]] PrimeCaps decode_prime_caps(std::uint64_t cap) noexcept;

// True for connector types that can hold their image without a flip (eDP / DSI).
// Pure helper behind DriverProfile::psr, exposed for testing.
[[nodiscard]] bool connector_type_self_refreshes(std::uint32_t connector_type) noexcept;

}  // namespace drm::display
