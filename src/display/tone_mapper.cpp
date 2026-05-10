// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "tone_mapper.hpp"

#include "color_pipeline_curves.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace drm::display {

namespace {

// Linear-light RGB matrices (ITU-R BT.2087, BT.2087-1 inverse).
struct Matrix3x3 {
  std::array<double, 9> m;
};

constexpr Matrix3x3 k_bt709_to_bt2020{{
    0.6274,
    0.3293,
    0.0433,  //
    0.0691,
    0.9195,
    0.0114,  //
    0.0164,
    0.0880,
    0.8956,
}};

constexpr Matrix3x3 k_bt2020_to_bt709{{
    1.6605,
    -0.5876,
    -0.0728,  //
    -0.1246,
    1.1329,
    -0.0083,  //
    -0.0182,
    -0.1006,
    1.1187,
}};

// BT.2020 luminance weights (Y' = 0.2627·R + 0.6780·G + 0.0593·B).
constexpr double k_bt2020_y_r = 0.2627;
constexpr double k_bt2020_y_g = 0.6780;
constexpr double k_bt2020_y_b = 0.0593;

// LUT step: 1024 buckets across [0, 1] of linear or encoded value.
// Bottom 6 bits of the u16 channel index drive interpolation.
constexpr std::uint32_t k_lut_buckets = 1024;
constexpr std::uint32_t k_input_index_shift = 6U;  // u16 >> 6 = top 10 bits
constexpr double k_input_frac_div = 64.0;          // bottom 6 bits / 64
constexpr double k_output_scale = static_cast<double>(k_lut_buckets);

// ── Tone-map curves (cheap arithmetic, no LUT needed) ───────────────

[[nodiscard]] constexpr double reinhard_tonemap(double x) noexcept {
  return x / (1.0 + x);
}

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

// ── LUT lookup with linear interpolation ───────────────────────────

/// Look up a u16 encoded channel value in the input LUT. The top
/// 10 bits select the LUT bucket, the bottom 6 drive linear
/// interpolation between buckets.
[[nodiscard]] double sample_input_lut(const std::array<float, ToneMapper::k_lut_size>& lut,
                                      std::uint16_t encoded) noexcept {
  const std::uint32_t idx = encoded >> k_input_index_shift;
  const double frac = static_cast<double>(encoded & 0x3FU) / k_input_frac_div;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
  return lut[idx] + (frac * (lut[idx + 1] - lut[idx]));
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
}

/// Look up a normalized linear value in the output LUT. The value
/// is scaled by 1024 to produce a fractional index; integer part
/// selects the bucket, fractional part interpolates.
[[nodiscard]] double sample_output_lut(const std::array<float, ToneMapper::k_lut_size>& lut,
                                       double linear) noexcept {
  linear = std::clamp(linear, 0.0, 1.0);
  const double scaled = linear * k_output_scale;
  const auto idx = static_cast<std::uint32_t>(scaled);
  const std::uint32_t safe_idx = std::min(idx, k_lut_buckets);  // protect against linear == 1.0
  const double frac = scaled - static_cast<double>(safe_idx);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
  if (safe_idx >= k_lut_buckets) {
    return lut[k_lut_buckets];
  }
  return lut[safe_idx] + (frac * (lut[safe_idx + 1] - lut[safe_idx]));
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
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
      target_max_nits_(target_max) {
  build_luts();
}

void ToneMapper::build_luts() noexcept {
  // Input LUT entries map encoded `[0, 1]` (`i / k_lut_buckets`)
  // to linear-light. Output LUT entries map linear `[0, 1]` to
  // encoded. Both LUTs are built once at construction.
  for (std::uint32_t i = 0; i < k_lut_size; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(k_lut_buckets);

    // Input curve depends on direction.
    double in_v = 0.0;
    switch (direction_) {
      case Direction::Bt709ToBt2020Pq:
        in_v = srgb_eotf(t);
        break;
      case Direction::Bt2020PqToBt709:
        in_v = pq_eotf(t);
        break;
      case Direction::HlgToBt709:
        in_v = hlg_oetf_inverse(t);
        break;
    }
    input_lut_.at(i) = static_cast<float>(in_v);

    // Output curve.
    double out_v = 0.0;
    switch (direction_) {
      case Direction::Bt709ToBt2020Pq:
        out_v = pq_oetf(t);
        break;
      case Direction::Bt2020PqToBt709:
      case Direction::HlgToBt709:
        out_v = bt1886_oetf(t);
        break;
    }
    output_lut_.at(i) = static_cast<float>(out_v);
  }
}

ToneMapper ToneMapper::bt709_to_bt2020_pq(const float sdr_white_nits) noexcept {
  return {Direction::Bt709ToBt2020Pq, sdr_white_nits, /*target_max=*/0.0F, ToneMapCurve::None};
}

ToneMapper ToneMapper::bt2020_pq_to_bt709(const float target_max_nits,
                                          const ToneMapCurve curve) noexcept {
  return {Direction::Bt2020PqToBt709, /*sdr_white=*/0.0F, target_max_nits, curve};
}

ToneMapper ToneMapper::hlg_to_bt709(const float target_max_nits) noexcept {
  return {Direction::HlgToBt709, /*sdr_white=*/0.0F, target_max_nits, ToneMapCurve::Reinhard};
}

void ToneMapper::apply(const std::uint64_t* src, std::uint64_t* dst,
                       const std::size_t count) const noexcept {
  // Per-direction dispatch hoisted outside the loop so the inner
  // body branches only on data, not on configuration. The compiler
  // is more willing to vectorize a tight loop where the work is
  // direction-uniform.
  for (std::size_t i = 0; i < count; ++i) {
    dst[i] = operator()(src[i]);
  }
}

std::uint64_t ToneMapper::operator()(const std::uint64_t input_pixel) const noexcept {
  // Unpack four u16 channels, little-endian. Alpha is preserved
  // verbatim — tone-mapping is color-only.
  const auto r_q = static_cast<std::uint16_t>(input_pixel & 0xFFFFU);
  const auto g_q = static_cast<std::uint16_t>((input_pixel >> 16U) & 0xFFFFU);
  const auto b_q = static_cast<std::uint16_t>((input_pixel >> 32U) & 0xFFFFU);
  const auto a_q = static_cast<std::uint16_t>((input_pixel >> 48U) & 0xFFFFU);

  // input LUT — encoded → linear. The transcendental
  // is precomputed; per-pixel cost is one bucket lookup + one
  // fma per channel.
  double r = sample_input_lut(input_lut_, r_q);
  double g = sample_input_lut(input_lut_, g_q);
  double b = sample_input_lut(input_lut_, b_q);

  // matrix + scale + tone-map (direction-specific).
  switch (direction_) {
    case Direction::Bt709ToBt2020Pq: {
      const auto rgb = matrix_mul(k_bt709_to_bt2020, {r, g, b});
      const double scale = static_cast<double>(sdr_white_nits_) / 10000.0;
      r = std::clamp(std::max(0.0, rgb[0]) * scale, 0.0, 1.0);
      g = std::clamp(std::max(0.0, rgb[1]) * scale, 0.0, 1.0);
      b = std::clamp(std::max(0.0, rgb[2]) * scale, 0.0, 1.0);
      break;
    }
    case Direction::Bt2020PqToBt709: {
      const auto rgb = matrix_mul(k_bt2020_to_bt709, {r, g, b});
      const double scale_to_target = 10000.0 / static_cast<double>(target_max_nits_);
      r = apply_tone_curve(std::max(0.0, rgb[0]) * scale_to_target, curve_);
      g = apply_tone_curve(std::max(0.0, rgb[1]) * scale_to_target, curve_);
      b = apply_tone_curve(std::max(0.0, rgb[2]) * scale_to_target, curve_);
      break;
    }
    case Direction::HlgToBt709: {
      // BT.2100 OOTF: scene → display-linear via system-gamma factor.
      const double y = (k_bt2020_y_r * r) + (k_bt2020_y_g * g) + (k_bt2020_y_b * b);
      // Use std::pow once; the OOTF gamma factor isn't a per-channel
      // transcendental so it can't go through the input LUT.
      const double ootf = std::pow(std::max(y, 0.0), 0.2);
      r = std::max(0.0, r * ootf);
      g = std::max(0.0, g * ootf);
      b = std::max(0.0, b * ootf);
      const auto rgb = matrix_mul(k_bt2020_to_bt709, {r, g, b});
      const double scale_to_target = 12.0 / (static_cast<double>(target_max_nits_) / 100.0);
      r = reinhard_tonemap(std::max(0.0, rgb[0]) * scale_to_target);
      g = reinhard_tonemap(std::max(0.0, rgb[1]) * scale_to_target);
      b = reinhard_tonemap(std::max(0.0, rgb[2]) * scale_to_target);
      break;
    }
  }

  // output LUT — linear → encoded.
  r = sample_output_lut(output_lut_, r);
  g = sample_output_lut(output_lut_, g);
  b = sample_output_lut(output_lut_, b);

  const auto to_u16 = [](double v) noexcept {
    return static_cast<std::uint16_t>(std::round(std::clamp(v, 0.0, 1.0) * 65535.0));
  };
  return static_cast<std::uint64_t>(to_u16(r)) | (static_cast<std::uint64_t>(to_u16(g)) << 16U) |
         (static_cast<std::uint64_t>(to_u16(b)) << 32U) | (static_cast<std::uint64_t>(a_q) << 48U);
}

}  // namespace drm::display
