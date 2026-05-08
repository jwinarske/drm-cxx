// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// output_signaling.hpp — derive connector-side colorimetry +
// HDR signaling from a set of `DisplayParams`.
//
// Producer side (DisplayParams) describes per-layer container
// colorimetry + EOTF. Consumer side (kernel connector properties)
// wants a single `Colorspace` enum value plus an optional
// `HDR_OUTPUT_METADATA` blob describing the mastering display. The
// helpers here bridge the two:
//
//   * `color_primaries_to_colorimetry` — convert a `ColorPrimaries`
//     enum to canonical CIE 1931 chromaticity coordinates, suitable
//     for seeding `HdrSourceMetadata::display_primaries`.
//   * `widest_gamut` — pick the widest-gamut entry from a span of
//     `DisplayParams` (BT.2020 > DCI-P3 > Adobe RGB > BT.709), so the
//     scene can advertise that container colorimetry on the
//     connector even if only one layer needs it.
//
// LayerScene auto-derive integration is a follow-up slice; these
// helpers are the building blocks.

#pragma once

#include "display_params.hpp"

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/connector_capabilities.hpp>
#include <drm-cxx/display/connector_info.hpp>
#include <drm-cxx/display/hdr_metadata.hpp>

#include <optional>

namespace drm::scene {

/// What the scene wants to declare on the output connector for a
/// given frame's set of layers. Both fields are optional: nullopt
/// `colorspace` means "leave the connector property at its current
/// value" (typically the driver's default SDR signaling); nullopt
/// `hdr_metadata` means "no HDR signaling" (clear via blob_id 0).
struct OutputSignalling {
  std::optional<drm::display::Colorspace> colorspace;
  std::optional<drm::display::HdrSourceMetadata> hdr_metadata;
};

/// Canonical CIE 1931 chromaticity coordinates for a `ColorPrimaries`
/// enum. White point is D65 for BT.709, BT.2020, and DCI-P3 D65;
/// Adobe RGB also uses D65. Returns `has_primaries == true` and
/// `has_default_white == true` so callers can hand the result
/// straight to `HdrSourceMetadata::display_primaries`.
[[nodiscard]] drm::display::ColorimetryInfo color_primaries_to_colorimetry(
    ColorPrimaries cp) noexcept;

/// Pick the widest-gamut `ColorPrimaries` from a set of
/// `DisplayParams`. Order: BT.2020 > DCI-P3 > Adobe RGB > BT.709.
/// Returns `nullopt` when no layer carries `color_primaries`
/// (caller falls back to BT.709 default).
[[nodiscard]] std::optional<ColorPrimaries> widest_gamut(
    drm::span<const DisplayParams* const> layers) noexcept;

/// Map a producer-side `ColorPrimaries` to a kernel-side
/// `drm::display::Colorspace` enum. The mapping is:
///   * Bt709    → Default (let the driver pick its standard SDR
///     signaling — explicit BT709_YCC would force YCC encoding,
///     which isn't the right default for RGB sources)
///   * Bt2020   → Bt2020Rgb (the RGB variant is the typical
///     desktop / signage path; YCC content uses the plane-side
///     COLOR_ENCODING property instead)
///   * DciP3    → DciP3RgbD65
///   * AdobeRgb → OpRgb
[[nodiscard]] drm::display::Colorspace color_primaries_to_colorspace(ColorPrimaries cp) noexcept;

/// Derive what the connector should signal for a frame whose live
/// layers are `layers`. Drives auto-population of the connector's
/// `Colorspace` property and `HDR_OUTPUT_METADATA` blob when the
/// caller hasn't overridden them via the manual scene API.
///
/// Rules:
///
///   * The widest-gamut layer's `color_primaries` (per
///     `widest_gamut()`) maps through `color_primaries_to_colorspace`.
///     If no layer carries `color_primaries`, `colorspace` is
///     nullopt (caller leaves the connector property alone).
///   * If any layer's `source_eotf` is HDR (PQ or HLG),
///     `hdr_metadata` is populated. PQ wins over HLG when both
///     are present (PQ is the more common HDR10 path). The
///     metadata's mastering display primaries are seeded from the
///     widest-gamut layer (BT.2020 default if no layer specifies);
///     the luminance / MaxCLL / MaxFALL fields stay at 0 — the
///     scene's manual `set_output_metadata` overrides this entire
///     struct when set, so callers with mastering data feed it
///     through that path.
[[nodiscard]] OutputSignalling derive_output_signaling(
    drm::span<const DisplayParams* const> layers) noexcept;

}  // namespace drm::scene
