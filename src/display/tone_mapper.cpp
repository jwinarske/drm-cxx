// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "tone_mapper.hpp"

#include "color_pipeline_curves.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace drm::display {

namespace {

// Linear-light RGB matrices (ITU-R BT.2087, BT.2087-1 inverse).
struct Matrix3x3 {
  std::array<float, 9> m;

  [[nodiscard]] constexpr std::array<float, 3> mul(const std::array<float, 3> v) const noexcept {
    return {(m[0] * v[0]) + (m[1] * v[1]) + (m[2] * v[2]),
            (m[3] * v[0]) + (m[4] * v[1]) + (m[5] * v[2]),
            (m[6] * v[0]) + (m[7] * v[1]) + (m[8] * v[2])};
  }
};

constexpr Matrix3x3 k_bt709_to_bt2020{{
    0.6274F,
    0.3293F,
    0.0433F,  //
    0.0691F,
    0.9195F,
    0.0114F,  //
    0.0164F,
    0.0880F,
    0.8956F,
}};

constexpr Matrix3x3 k_bt2020_to_bt709{{
    1.6605F,
    -0.5876F,
    -0.0728F,  //
    -0.1246F,
    1.1329F,
    -0.0083F,  //
    -0.0182F,
    -0.1006F,
    1.1187F,
}};

// BT.2020 luminance weights (Y' = 0.2627·R + 0.6780·G + 0.0593·B).
constexpr float k_bt2020_y_r = 0.2627F;
constexpr float k_bt2020_y_g = 0.6780F;
constexpr float k_bt2020_y_b = 0.0593F;

// Reinhard tone-map: `x / (1 + x)`. Asymptotic: `x = ∞` → output `1`.
[[nodiscard]] constexpr double reinhard_tonemap(double x) noexcept {
  return x / (1.0 + x);
}

// Uncharted 2 / John Hable filmic curve — ACES-ish S-shape. The
// constants are the published "filmic_2007" values; a final divide
// by the curve evaluated at the spec's reference white point
// (`11.2`) re-normalizes the output to `[0, 1]`.
[[nodiscard]] double hable_curve(double x) noexcept {
  constexpr double a = 0.15;
  constexpr double b = 0.50;
  constexpr double c = 0.10;
  constexpr double d = 0.20;
  constexpr double e = 0.02;
  constexpr double f = 0.30;
  return (((x * ((a * x) + (c * b))) + (d * e)) / ((x * ((a * x) + b)) + (d * f))) - (e / f);
}

[[nodiscard]] double hable_tonemap(double x) noexcept {
  constexpr double k_white = 11.2;
  const double mapped = hable_curve(x);
  const double normalize = hable_curve(k_white);
  return std::clamp(mapped / normalize, 0.0, 1.0);
}

[[nodiscard]] double apply_tone_curve(double x, ToneMapCurve curve) noexcept {
  switch (curve) {
    case ToneMapCurve::None:
      return std::clamp(x, 0.0, 1.0);
    case ToneMapCurve::Reinhard:
      return reinhard_tonemap(std::max(x, 0.0));
    case ToneMapCurve::Hable:
      return hable_tonemap(std::max(x, 0.0));
  }
  return std::clamp(x, 0.0, 1.0);
}

[[nodiscard]] std::uint16_t to_u16(double v) noexcept {
  return static_cast<std::uint16_t>(std::round(std::clamp(v, 0.0, 1.0) * 65535.0));
}

[[nodiscard]] std::array<double, 3> matrix_mul(const Matrix3x3& m,
                                               std::array<double, 3> v) noexcept {
  return {(m.m[0] * v[0]) + (m.m[1] * v[1]) + (m.m[2] * v[2]),
          (m.m[3] * v[0]) + (m.m[4] * v[1]) + (m.m[5] * v[2]),
          (m.m[6] * v[0]) + (m.m[7] * v[1]) + (m.m[8] * v[2])};
}

}  // namespace

ToneMapper::ToneMapper(const Direction direction, const float sdr_white, const float target_max,
                       const ToneMapCurve curve) noexcept
    : direction_(direction),
      curve_(curve),
      sdr_white_nits_(sdr_white),
      target_max_nits_(target_max) {}

ToneMapper ToneMapper::bt709_to_bt2020_pq(const float sdr_white_nits) noexcept {
  return {Direction::Bt709ToBt2020Pq, sdr_white_nits, /*target_max=*/0.0F, ToneMapCurve::None};
}

ToneMapper ToneMapper::bt2020_pq_to_bt709(const float target_max_nits,
                                          const ToneMapCurve curve) noexcept {
  return {Direction::Bt2020PqToBt709, /*sdr_white=*/0.0F, target_max_nits, curve};
}

ToneMapper ToneMapper::hlg_to_bt709(const float target_max_nits) noexcept {
  // HLG path uses Reinhard implicitly during the matrix→OETF stage;
  // the curve member isn't user-selectable for HLG → SDR.
  return {Direction::HlgToBt709, /*sdr_white=*/0.0F, target_max_nits, ToneMapCurve::Reinhard};
}

std::uint64_t ToneMapper::operator()(const std::uint64_t input_pixel) const noexcept {
  // Unpack four u16 channels, little-endian. Alpha is preserved
  // verbatim — tone-mapping is color-only.
  const auto r_q = static_cast<std::uint16_t>(input_pixel & 0xFFFFU);
  const auto g_q = static_cast<std::uint16_t>((input_pixel >> 16U) & 0xFFFFU);
  const auto b_q = static_cast<std::uint16_t>((input_pixel >> 32U) & 0xFFFFU);
  const auto a_q = static_cast<std::uint16_t>((input_pixel >> 48U) & 0xFFFFU);

  double r = static_cast<double>(r_q) / 65535.0;
  double g = static_cast<double>(g_q) / 65535.0;
  double b = static_cast<double>(b_q) / 65535.0;

  switch (direction_) {
    case Direction::Bt709ToBt2020Pq: {
      // sRGB EOTF: encoded → linear (in [0, 1] = SDR full white).
      r = srgb_eotf(r);
      g = srgb_eotf(g);
      b = srgb_eotf(b);
      // BT.709 → BT.2020 in linear-light.
      const auto rgb = matrix_mul(k_bt709_to_bt2020, {r, g, b});
      r = std::max(0.0, rgb[0]);
      g = std::max(0.0, rgb[1]);
      b = std::max(0.0, rgb[2]);
      // Scale SDR full-white to its target luminance on the PQ curve.
      // PQ's `1.0` is 10 000 cd/m², so SDR full-white sits at
      // `sdr_white_nits / 10000`.
      const double scale = static_cast<double>(sdr_white_nits_) / 10000.0;
      r = std::clamp(r * scale, 0.0, 1.0);
      g = std::clamp(g * scale, 0.0, 1.0);
      b = std::clamp(b * scale, 0.0, 1.0);
      // PQ OETF: linear → encoded.
      r = pq_oetf(r);
      g = pq_oetf(g);
      b = pq_oetf(b);
      break;
    }
    case Direction::Bt2020PqToBt709: {
      // PQ EOTF: encoded → linear (10 000-cd/m² peak).
      r = pq_eotf(r);
      g = pq_eotf(g);
      b = pq_eotf(b);
      // BT.2020 → BT.709 in linear-light.
      const auto rgb = matrix_mul(k_bt2020_to_bt709, {r, g, b});
      r = std::max(0.0, rgb[0]);
      g = std::max(0.0, rgb[1]);
      b = std::max(0.0, rgb[2]);
      // Tone-map: scale linear values so `target_max_nits / 10000`
      // maps to `1.0` on the curve, then run the curve. Out-of-range
      // values get rolled smoothly into [0, 1] by Reinhard / Hable.
      const double scale_to_target = 10000.0 / static_cast<double>(target_max_nits_);
      r = apply_tone_curve(r * scale_to_target, curve_);
      g = apply_tone_curve(g * scale_to_target, curve_);
      b = apply_tone_curve(b * scale_to_target, curve_);
      // BT.1886 OETF: linear → encoded.
      r = bt1886_oetf(r);
      g = bt1886_oetf(g);
      b = bt1886_oetf(b);
      break;
    }
    case Direction::HlgToBt709: {
      // HLG OETF^-1: encoded → scene-linear in nominal `[0, 12]`.
      r = hlg_oetf_inverse(r);
      g = hlg_oetf_inverse(g);
      b = hlg_oetf_inverse(b);
      // BT.2100 OOTF: scene → display-linear via system-gamma factor.
      // For an SDR target gamma of 2.4, the system gamma reduces to
      // 1.2 nominal; multiplying scene RGB by `Y^(γ-1)` with `γ=1.2`
      // gives the display-referred values. Y is the BT.2020 luma.
      const double y = (k_bt2020_y_r * r) + (k_bt2020_y_g * g) + (k_bt2020_y_b * b);
      const double ootf = std::pow(std::max(y, 0.0), 0.2);
      r = std::max(0.0, r * ootf);
      g = std::max(0.0, g * ootf);
      b = std::max(0.0, b * ootf);
      // BT.2020 → BT.709 (display-linear).
      const auto rgb = matrix_mul(k_bt2020_to_bt709, {r, g, b});
      r = std::max(0.0, rgb[0]);
      g = std::max(0.0, rgb[1]);
      b = std::max(0.0, rgb[2]);
      // Reinhard to compress remaining [0, ~12 nominal] into [0, 1].
      // Scale by 12 / target so target_max_nits=100 leaves SDR sitting
      // at the curve's `0.5` knee.
      const double scale_to_target = 12.0 / (static_cast<double>(target_max_nits_) / 100.0);
      r = reinhard_tonemap(r * scale_to_target);
      g = reinhard_tonemap(g * scale_to_target);
      b = reinhard_tonemap(b * scale_to_target);
      // BT.1886 OETF.
      r = bt1886_oetf(r);
      g = bt1886_oetf(g);
      b = bt1886_oetf(b);
      break;
    }
  }

  return static_cast<std::uint64_t>(to_u16(r)) | (static_cast<std::uint64_t>(to_u16(g)) << 16U) |
         (static_cast<std::uint64_t>(to_u16(b)) << 32U) | (static_cast<std::uint64_t>(a_q) << 48U);
}

}  // namespace drm::display
