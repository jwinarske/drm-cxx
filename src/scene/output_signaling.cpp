// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "output_signaling.hpp"

#include "display_params.hpp"

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/connector_info.hpp>

#include <cstdint>
#include <optional>

namespace drm::scene {

namespace {

// Per-enum gamut rank. Higher value = wider gamut. Used to pick the
// connector Colorspace from the layer with the widest container
// colorimetry. The order is the one called out in the design
// (BT.2020 > DCI-P3 > Adobe RGB > BT.709) — strictly by the area
// of the chromaticity triangle the primaries enclose, BT.2020 is
// the largest of these four, then DCI-P3, then Adobe RGB, then
// BT.709 (sRGB).
constexpr std::uint8_t gamut_rank(ColorPrimaries cp) noexcept {
  switch (cp) {
    case ColorPrimaries::Bt709:
      return 0;
    case ColorPrimaries::AdobeRgb:
      return 1;
    case ColorPrimaries::DciP3:
      return 2;
    case ColorPrimaries::Bt2020:
      return 3;
  }
  return 0;
}

}  // namespace

drm::display::ColorimetryInfo color_primaries_to_colorimetry(const ColorPrimaries cp) noexcept {
  drm::display::ColorimetryInfo info;
  info.has_primaries = true;
  info.has_default_white = true;
  switch (cp) {
    case ColorPrimaries::Bt709:
      // BT.709 / sRGB primaries + D65 (ITU-R BT.709-6 §3.2).
      info.red = {0.640F, 0.330F};
      info.green = {0.300F, 0.600F};
      info.blue = {0.150F, 0.060F};
      info.white = {0.3127F, 0.3290F};
      break;
    case ColorPrimaries::Bt2020:
      // BT.2020 / Rec.2100 primaries + D65 (ITU-R BT.2020-2 §2.3).
      info.red = {0.708F, 0.292F};
      info.green = {0.170F, 0.797F};
      info.blue = {0.131F, 0.046F};
      info.white = {0.3127F, 0.3290F};
      break;
    case ColorPrimaries::DciP3:
      // DCI-P3 D65 (display-referred consumer P3, SMPTE EG 432-1).
      info.red = {0.680F, 0.320F};
      info.green = {0.265F, 0.690F};
      info.blue = {0.150F, 0.060F};
      info.white = {0.3127F, 0.3290F};
      break;
    case ColorPrimaries::AdobeRgb:
      // Adobe RGB (1998) + D65 (Adobe RGB 1998 spec §4.3.4.1).
      info.red = {0.640F, 0.330F};
      info.green = {0.210F, 0.710F};
      info.blue = {0.150F, 0.060F};
      info.white = {0.3127F, 0.3290F};
      break;
  }
  return info;
}

std::optional<ColorPrimaries> widest_gamut(
    const drm::span<const DisplayParams* const> layers) noexcept {
  std::optional<ColorPrimaries> winner;
  std::uint8_t winner_rank = 0;
  for (const auto* layer : layers) {
    if (layer == nullptr || !layer->color_primaries.has_value()) {
      continue;
    }
    const auto rank = gamut_rank(*layer->color_primaries);
    if (!winner.has_value() || rank > winner_rank) {
      winner = layer->color_primaries;
      winner_rank = rank;
    }
  }
  return winner;
}

}  // namespace drm::scene
