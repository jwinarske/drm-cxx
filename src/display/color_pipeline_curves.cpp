// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "color_pipeline_curves.hpp"

#include <drm-cxx/detail/span.hpp>

#include <drm/drm_mode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace drm::display {

namespace {

constexpr std::uint64_t k_s31_32_one = 1ULL << 32U;  // == 1.0
constexpr std::uint64_t k_s31_32_sign_bit = 1ULL << 63U;
constexpr std::uint64_t k_s31_32_max_magnitude = (1ULL << 63U) - 1U;

// SMPTE ST 2084 (PQ) constants. Numerators / denominators kept in
// fractional form so the resulting double is bit-exact with the
// spec's 12-bit reference implementation.
constexpr double k_pq_m1 = 2610.0 / 16384.0;       // 0.15930175...
constexpr double k_pq_m2 = 2523.0 / 4096.0 * 128;  // 78.84375
constexpr double k_pq_c1 = 3424.0 / 4096.0;        // 0.8359375
constexpr double k_pq_c2 = 2413.0 / 4096.0 * 32;   // 18.8515625
constexpr double k_pq_c3 = 2392.0 / 4096.0 * 32;   // 18.6875

// ITU-R BT.2100 HLG OETF constants.
constexpr double k_hlg_a = 0.17883277;
constexpr double k_hlg_b = 0.28466892;
constexpr double k_hlg_c = 0.55991073;

// Fill the LUT in `out` with `f(i / (N - 1))` per entry. `f` is
// invoked many times so it's taken by value; templating is what
// keeps the per-call indirection inlinable.
template <typename F>
void fill_lut(drm::span<drm_color_lut> out, F f) noexcept {
  if (out.empty()) {
    return;
  }
  const auto n = out.size();
  const auto last = static_cast<double>(n - 1);
  for (std::size_t i = 0; i < n; ++i) {
    const double normalized = (n == 1) ? 0.0 : (static_cast<double>(i) / last);
    const std::uint16_t v = quantize_lut_value(f(normalized));
    out[i].red = v;
    out[i].green = v;
    out[i].blue = v;
    out[i].reserved = 0;
  }
}

void encode_matrix(const std::array<double, 9>& src, drm_color_ctm& out) noexcept {
  for (std::size_t i = 0; i < 9; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) — i bounded by loop
    out.matrix[i] = encode_s31_32(src[i]);
  }
}

}  // namespace

std::uint64_t encode_s31_32(double v) noexcept {
  if (std::isnan(v)) {
    return 0;
  }
  const bool negative = v < 0.0;
  const double mag = std::abs(v);
  // Saturate: anything above (2^31 - 2^-32) becomes the largest
  // representable magnitude. The kernel's tolerance for slight
  // overflow is undefined, so clamp explicitly.
  const double scaled = std::round(mag * static_cast<double>(k_s31_32_one));
  std::uint64_t magnitude = 0;
  if (scaled >= static_cast<double>(k_s31_32_max_magnitude)) {
    magnitude = k_s31_32_max_magnitude;
  } else {
    magnitude = static_cast<std::uint64_t>(scaled);
  }
  return magnitude | (negative ? k_s31_32_sign_bit : 0ULL);
}

double decode_s31_32(std::uint64_t encoded) noexcept {
  const bool negative = (encoded & k_s31_32_sign_bit) != 0U;
  const std::uint64_t magnitude = encoded & ~k_s31_32_sign_bit;
  const double mag = static_cast<double>(magnitude) / static_cast<double>(k_s31_32_one);
  return negative ? -mag : mag;
}

std::uint16_t quantize_lut_value(double normalized) noexcept {
  if (std::isnan(normalized) || normalized <= 0.0) {
    return 0;
  }
  if (normalized >= 1.0) {
    return 0xFFFFU;
  }
  return static_cast<std::uint16_t>(std::round(normalized * 65535.0));
}

double pq_eotf(double encoded) noexcept {
  encoded = std::clamp(encoded, 0.0, 1.0);
  const double n = std::pow(encoded, 1.0 / k_pq_m2);
  const double num = std::max(n - k_pq_c1, 0.0);
  const double den = k_pq_c2 - (k_pq_c3 * n);
  if (den <= 0.0) {
    return 0.0;
  }
  return std::pow(num / den, 1.0 / k_pq_m1);
}

double pq_oetf(double linear) noexcept {
  linear = std::clamp(linear, 0.0, 1.0);
  const double n = std::pow(linear, k_pq_m1);
  return std::pow((k_pq_c1 + (k_pq_c2 * n)) / (1.0 + (k_pq_c3 * n)), k_pq_m2);
}

double hlg_oetf_inverse(double encoded) noexcept {
  encoded = std::clamp(encoded, 0.0, 1.0);
  if (encoded <= 0.5) {
    return (encoded * encoded) / 3.0;
  }
  return (std::exp((encoded - k_hlg_c) / k_hlg_a) + k_hlg_b) / 12.0;
}

// IEC 61966-2-1 sRGB EOTF (encoded → linear). Two-segment piecewise:
// linear segment for the low end (where the gamma curve has zero
// slope) and gamma-2.4-ish above the breakpoint. Used as the SDR
// source curve when input content is BT.709 / sRGB.
double srgb_eotf(double encoded) noexcept {
  encoded = std::clamp(encoded, 0.0, 1.0);
  if (encoded <= 0.04045) {
    return encoded / 12.92;
  }
  return std::pow((encoded + 0.055) / 1.055, 2.4);
}

double srgb_oetf(double linear) noexcept {
  linear = std::clamp(linear, 0.0, 1.0);
  if (linear <= 0.0031308) {
    return 12.92 * linear;
  }
  return (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055;
}

// ITU-R BT.1886 OETF — fixed gamma 2.4. Black-level offset is
// omitted; most displays handle it themselves and the offset
// matters only for true 0-nit blacks which scanout typically
// can't hit anyway.
double bt1886_oetf(double linear) noexcept {
  return std::pow(std::max(linear, 0.0), 1.0 / 2.4);
}

void build_identity_lut(drm::span<drm_color_lut> out) noexcept {
  fill_lut(out, [](double v) { return v; });
}

void build_pq_eotf_lut(drm::span<drm_color_lut> out) noexcept {
  fill_lut(out, pq_eotf);
}

void build_pq_oetf_lut(drm::span<drm_color_lut> out) noexcept {
  fill_lut(out, pq_oetf);
}

void build_hlg_oetf_inverse_lut(drm::span<drm_color_lut> out) noexcept {
  fill_lut(out, hlg_oetf_inverse);
}

drm_color_ctm build_identity_ctm() noexcept {
  drm_color_ctm out{};
  out.matrix[0] = k_s31_32_one;
  out.matrix[4] = k_s31_32_one;
  out.matrix[8] = k_s31_32_one;
  return out;
}

drm_color_ctm build_bt2020_to_bt709_ctm() noexcept {
  // ITU-R BT.2087 reference matrix. Values are exact published
  // coefficients to 4 decimal places; the kernel quantizes them
  // further on its way to per-channel pipe registers.
  constexpr std::array<double, 9> k_bt2020_to_bt709{
      // clang-format off
       1.6605, -0.5876, -0.0728,
      -0.1246,  1.1329, -0.0083,
      -0.0182, -0.1006,  1.1187,
      // clang-format on
  };
  drm_color_ctm out{};
  encode_matrix(k_bt2020_to_bt709, out);
  return out;
}

drm_color_ctm build_bt709_to_bt2020_ctm() noexcept {
  // ITU-R BT.2087-1 (the inverse of the BT.2020→BT.709 matrix).
  constexpr std::array<double, 9> k_bt709_to_bt2020{
      // clang-format off
      0.6274, 0.3293, 0.0433,
      0.0691, 0.9195, 0.0114,
      0.0164, 0.0880, 0.8956,
      // clang-format on
  };
  drm_color_ctm out{};
  encode_matrix(k_bt709_to_bt2020, out);
  return out;
}

}  // namespace drm::display
