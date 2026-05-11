// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// connector_capabilities.hpp — KMS connector property capability cache
// for the HDR / wide-gamut output path.
//
// `ConnectorCapabilities` mirrors `drm::planes::PlaneCapabilities`:
// it caches the integer values the driver assigns to the named enum
// entries on a connector (`Colorspace`, `Broadcast RGB`), the range
// bounds on `max bpc`, and a presence flag for the `HDR_OUTPUT_METADATA`
// blob property. Callers writing these properties via an atomic
// commit look up the cached integer here rather than re-parsing the
// kernel's enum table every frame.
//

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <optional>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::display {

/// Connector `Colorspace` enum values. The kernel exposes these as
/// named entries on a per-connector enum property; the integer the
/// driver assigns to each name is driver-defined, so the cached
/// `colorspace_*` optionals on `ConnectorCapabilities` carry the
/// per-connector mapping. Names match the kernel's
/// `drm_dp_colorspaces` / `hdmi_colorspaces` property tables.
enum class Colorspace : std::uint8_t {
  Default,
  SmpteRf170mYcc,   // "SMPTE_170M_YCC"
  Bt709Ycc,         // "BT709_YCC"
  XvYcc601,         // "XVYCC_601"
  XvYcc709,         // "XVYCC_709"
  SYcc601,          // "SYCC_601"
  OpYcc601,         // "opYCC_601"
  OpRgb,            // "opRGB"
  Bt2020Cycc,       // "BT2020_CYCC"
  Bt2020Rgb,        // "BT2020_RGB"
  Bt2020Ycc,        // "BT2020_YCC"
  DciP3RgbD65,      // "DCI-P3_RGB_D65"
  DciP3RgbTheater,  // "DCI-P3_RGB_Theater"
  RgbWideFixed,     // "RGB_Wide_Gamut_Fixed_Point"
  RgbWideFloat,     // "RGB_Wide_Gamut_Floating_Point"
};

/// Connector `Broadcast RGB` enum values. RGB-quantization range,
/// orthogonal to YCbCr `COLOR_RANGE` on plane properties.
enum class BroadcastRgb : std::uint8_t {
  Automatic,  // "Automatic"
  Full,       // "Full"
  Limited,    // "Limited 16:235"
};

/// Cached HDR / colorimetry property capabilities for one connector.
/// Populated by `probe_connector_capabilities`; can also be constructed
/// directly by tests / synthetic fixtures.
struct ConnectorCapabilities {
  std::uint32_t connector_id{};

  /// True when the connector exposes the `Colorspace` enum property.
  /// Most modern Intel / amdgpu / i915 connectors do; some older / DP
  /// MST chains may not.
  bool has_colorspace{false};

  /// Cached enum integer for each `Colorspace` entry. nullopt when the
  /// connector lacks that specific entry — the kernel only reports
  /// values its hardware actually supports, so e.g. older amdgpu
  /// connectors expose Default + BT709_YCC + BT2020_RGB + BT2020_YCC
  /// but not the DCI-P3 entries. Callers must check before writing.
  std::optional<std::uint64_t> colorspace_default;
  std::optional<std::uint64_t> colorspace_smpte170m_ycc;
  std::optional<std::uint64_t> colorspace_bt709_ycc;
  std::optional<std::uint64_t> colorspace_xvycc_601;
  std::optional<std::uint64_t> colorspace_xvycc_709;
  std::optional<std::uint64_t> colorspace_sycc_601;
  std::optional<std::uint64_t> colorspace_opycc_601;
  std::optional<std::uint64_t> colorspace_oprgb;
  std::optional<std::uint64_t> colorspace_bt2020_cycc;
  std::optional<std::uint64_t> colorspace_bt2020_rgb;
  std::optional<std::uint64_t> colorspace_bt2020_ycc;
  std::optional<std::uint64_t> colorspace_dci_p3_rgb_d65;
  std::optional<std::uint64_t> colorspace_dci_p3_rgb_theater;
  std::optional<std::uint64_t> colorspace_rgb_wide_fixed;
  std::optional<std::uint64_t> colorspace_rgb_wide_float;

  /// True when the connector exposes the `max bpc` range property.
  /// Drives the maximum bits-per-component the driver will negotiate
  /// at the sink. Required ≥ 10 for HDR PQ output to actually be HDR
  /// rather than 8-bit-tone-mapped-with-a-PQ-flag.
  bool has_max_bpc{false};
  std::optional<std::uint64_t> max_bpc_min;      // typically 6 or 8
  std::optional<std::uint64_t> max_bpc_max;      // typically 8 / 10 / 12 / 16
  std::optional<std::uint64_t> max_bpc_current;  // current property value

  /// True when the connector exposes the `HDR_OUTPUT_METADATA` blob
  /// property. The blob is built and managed via the source-side
  /// `HdrMetadataBlob` helper; this flag is a presence
  /// check, so callers can detect HDR-incapable connectors up front.
  bool has_hdr_output_metadata{false};

  /// True when the connector exposes the `Broadcast RGB` enum
  /// property (HDMI sinks; not present on most DP). Cached enum
  /// integers per name follow the same contract as `colorspace_*`.
  bool has_broadcast_rgb{false};
  std::optional<std::uint64_t> broadcast_rgb_automatic;
  std::optional<std::uint64_t> broadcast_rgb_full;
  std::optional<std::uint64_t> broadcast_rgb_limited;

  /// Convenience: true when the connector can plausibly carry an HDR
  /// signal — needs `HDR_OUTPUT_METADATA` exposed *and* a `max bpc`
  /// ceiling of at least 10. Both are necessary; either alone gives
  /// 8-bit-with-an-HDR-flag, which the sink will accept but won't
  /// actually display as HDR.
  [[nodiscard]] bool can_signal_hdr() const noexcept;

  /// Look up the cached enum integer for `cs`. Returns nullopt when
  /// the connector lacks that specific Colorspace entry or when the
  /// `Colorspace` property isn't exposed at all.
  [[nodiscard]] std::optional<std::uint64_t> colorspace_value(Colorspace cs) const noexcept;

  /// As `colorspace_value`, for `Broadcast RGB`.
  [[nodiscard]] std::optional<std::uint64_t> broadcast_rgb_value(BroadcastRgb b) const noexcept;
};

/// Probe a connector's HDR-relevant property capabilities by walking
/// `drmModeObjectGetProperties(connector_id)` and caching the named
/// entries.  Returns `errc::no_such_device` when libdrm reports the
/// connector_id has no properties (kernel returned NULL); other errno
/// values are surfaced via `system_category`.  Properties not exposed
/// by the connector are simply absent from the returned struct — no
/// error.
drm::expected<ConnectorCapabilities, std::error_code> probe_connector_capabilities(
    const drm::Device& dev, std::uint32_t connector_id);

}  // namespace drm::display