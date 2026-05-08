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
#include <drm-cxx/display/connector_info.hpp>

#include <optional>

namespace drm::scene {

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

}  // namespace drm::scene
