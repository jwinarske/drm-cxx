// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// crtc_capabilities.hpp — KMS CRTC color-pipeline property cache.
//
// `CrtcCapabilities` mirrors `ConnectorCapabilities` for
// the CRTC-side color pipeline properties: `DEGAMMA_LUT` (input
// curve, EOTF inversion lives here), `CTM` (3×3 color-space
// matrix), `GAMMA_LUT` (output curve, OETF lives here), plus the
// immutable `*_LUT_SIZE` advertisements telling callers how many
// `drm_color_lut` entries the driver expects.
//
// Different drivers expose different subsets:
//   * amdgpu RDNA: all three with 4096-entry LUTs.
//   * i915 (Tigerlake+): all three with 1024-entry LUTs.
//   * vkms: GAMMA_LUT only (256 entries). No DEGAMMA / CTM.
//   * Older drivers: nothing — `CrtcColorPipeline::create()` returns
//     `errc::operation_not_supported` on those CRTCs.

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::display {

/// Cached CRTC color-pipeline property capabilities. Populated by
/// `probe_crtc_capabilities`. All `has_*` flags default false.
struct CrtcCapabilities {
  std::uint32_t crtc_id{};

  bool has_degamma_lut{false};
  bool has_ctm{false};
  bool has_gamma_lut{false};

  /// LUT entry counts (`drm_color_lut` rows). Zero when the
  /// corresponding `has_*_lut` is false. The kernel returns these
  /// as immutable range properties; the upper bound is the number
  /// of entries the driver expects in the blob.
  std::uint32_t degamma_lut_size{};
  std::uint32_t gamma_lut_size{};

  /// Convenience: true iff the CRTC exposes the full DEGAMMA + CTM
  /// + GAMMA pipeline. Drivers that expose only a subset (vkms with
  /// GAMMA-only, for instance) return false here even though
  /// individual stages may still be usable.
  [[nodiscard]] bool has_full_pipeline() const noexcept {
    return has_degamma_lut && has_ctm && has_gamma_lut;
  }
};

/// Probe a CRTC's color-pipeline property capabilities by walking
/// `drmModeObjectGetProperties(crtc_id)` and recording the named
/// entries. `errc::no_such_device` when libdrm reports the crtc_id
/// has no properties; other errno values surface via
/// `system_category`. Properties not exposed are simply absent
/// from the returned struct — no error.
[[nodiscard]] drm::expected<CrtcCapabilities, std::error_code> probe_crtc_capabilities(
    const drm::Device& dev, std::uint32_t crtc_id);

}  // namespace drm::display
