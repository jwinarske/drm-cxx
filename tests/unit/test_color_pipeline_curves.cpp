// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "display/color_pipeline_curves.hpp"

#include <drm-cxx/detail/span.hpp>

#include <drm/drm_mode.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace {

constexpr std::uint64_t k_s31_32_one = 1ULL << 32U;
constexpr std::uint64_t k_s31_32_sign = 1ULL << 63U;

// ── S31.32 sign-magnitude round-trip ────────────────────────────────

TEST(ColorPipelineCurves, EncodeS31_32Zero) {
  EXPECT_EQ(drm::display::encode_s31_32(0.0), 0U);
}

TEST(ColorPipelineCurves, EncodeS31_32One) {
  EXPECT_EQ(drm::display::encode_s31_32(1.0), k_s31_32_one);
}

TEST(ColorPipelineCurves, EncodeS31_32MinusOne) {
  EXPECT_EQ(drm::display::encode_s31_32(-1.0), k_s31_32_sign | k_s31_32_one);
}

TEST(ColorPipelineCurves, EncodeS31_32HalfRoundsUp) {
  // 0.5 in S31.32 == 0x80000000 (0.5 * 2^32).
  EXPECT_EQ(drm::display::encode_s31_32(0.5), 1ULL << 31U);
}

TEST(ColorPipelineCurves, EncodeS31_32NegativeHalfHasSignBit) {
  EXPECT_EQ(drm::display::encode_s31_32(-0.5), k_s31_32_sign | (1ULL << 31U));
}

TEST(ColorPipelineCurves, EncodeS31_32NaNYieldsZero) {
  EXPECT_EQ(drm::display::encode_s31_32(std::nan("")), 0U);
}

TEST(ColorPipelineCurves, DecodeS31_32RoundTrip) {
  for (const double v : {0.0, 1.0, -1.0, 0.5, -0.5, 2.5, -3.7, 0.1234, -0.9876}) {
    const auto encoded = drm::display::encode_s31_32(v);
    const auto decoded = drm::display::decode_s31_32(encoded);
    EXPECT_NEAR(decoded, v, 1.0 / static_cast<double>(k_s31_32_one)) << v;
  }
}

// ── LUT quantization ────────────────────────────────────────────────

TEST(ColorPipelineCurves, QuantizeLutValueClampsBelowZero) {
  EXPECT_EQ(drm::display::quantize_lut_value(-0.1), 0U);
  EXPECT_EQ(drm::display::quantize_lut_value(0.0), 0U);
}

TEST(ColorPipelineCurves, QuantizeLutValueClampsAboveOne) {
  EXPECT_EQ(drm::display::quantize_lut_value(1.0), 0xFFFFU);
  EXPECT_EQ(drm::display::quantize_lut_value(2.0), 0xFFFFU);
}

TEST(ColorPipelineCurves, QuantizeLutValueHalfIsMidpoint) {
  EXPECT_EQ(drm::display::quantize_lut_value(0.5),
            static_cast<std::uint16_t>(std::round(0.5 * 65535.0)));
}

// ── Identity LUT ────────────────────────────────────────────────────

TEST(ColorPipelineCurves, IdentityLutMonotonic) {
  std::vector<drm_color_lut> lut(256);
  drm::display::build_identity_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  for (std::size_t i = 1; i < lut.size(); ++i) {
    EXPECT_GE(lut[i].red, lut[i - 1].red);
    EXPECT_EQ(lut[i].red, lut[i].green);
    EXPECT_EQ(lut[i].red, lut[i].blue);
    EXPECT_EQ(lut[i].reserved, 0U);
  }
  EXPECT_EQ(lut.front().red, 0U);
  EXPECT_EQ(lut.back().red, 0xFFFFU);
}

TEST(ColorPipelineCurves, IdentityLutEmptySpanNoOp) {
  drm::display::build_identity_lut(drm::span<drm_color_lut>{});
  // No crash, no allocation.
}

// ── PQ EOTF ─────────────────────────────────────────────────────────

TEST(ColorPipelineCurves, PqEotfEndpoints) {
  std::vector<drm_color_lut> lut(4096);
  drm::display::build_pq_eotf_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  EXPECT_EQ(lut.front().red, 0U) << "PQ EOTF(0) should be 0";
  EXPECT_EQ(lut.back().red, 0xFFFFU) << "PQ EOTF(1) should be 1.0 (peak luminance)";
}

TEST(ColorPipelineCurves, PqEotfMonotonic) {
  std::vector<drm_color_lut> lut(4096);
  drm::display::build_pq_eotf_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  for (std::size_t i = 1; i < lut.size(); ++i) {
    EXPECT_GE(lut[i].red, lut[i - 1].red) << "non-monotonic at i=" << i;
  }
}

TEST(ColorPipelineCurves, PqEotfMatchesSpecAt100Nits) {
  // Per SMPTE ST 2084, the PQ encoded value for 100 cd/m² is
  // approximately 0.5081. Linear value at that input should be
  // 0.01 (= 100/10000).
  std::vector<drm_color_lut> lut(4096);
  drm::display::build_pq_eotf_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  // Look up the LUT entry whose normalized input is closest to
  // 0.5081. That's index round(0.5081 * 4095) = 2080.
  const auto idx = static_cast<std::size_t>(std::round(0.5081 * 4095.0));
  // Expected: linear ≈ 0.01 → quantized to ≈ 655 (0.01 * 65535).
  EXPECT_NEAR(lut[idx].red, 655U, 30U);
}

TEST(ColorPipelineCurves, PqEotfMatchesSpecAt1000Nits) {
  // PQ value for 1000 cd/m² ≈ 0.7518. Linear value ≈ 0.1.
  std::vector<drm_color_lut> lut(4096);
  drm::display::build_pq_eotf_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  const auto idx = static_cast<std::size_t>(std::round(0.7518 * 4095.0));
  // Expected: linear ≈ 0.1 → quantized to ≈ 6553.
  EXPECT_NEAR(lut[idx].red, 6553U, 60U);
}

// ── PQ OETF (linear → encoded) ──────────────────────────────────────

TEST(ColorPipelineCurves, PqOetfEndpoints) {
  std::vector<drm_color_lut> lut(4096);
  drm::display::build_pq_oetf_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  EXPECT_EQ(lut.front().red, 0U);
  EXPECT_EQ(lut.back().red, 0xFFFFU);
}

TEST(ColorPipelineCurves, PqOetfRoundTripWithEotf) {
  // Building EOTF then OETF and composing should be approximately
  // identity (within quantization). Sample at a handful of points.
  std::vector<drm_color_lut> eotf(4096);
  std::vector<drm_color_lut> oetf(4096);
  drm::display::build_pq_eotf_lut(drm::span<drm_color_lut>(eotf.data(), eotf.size()));
  drm::display::build_pq_oetf_lut(drm::span<drm_color_lut>(oetf.data(), oetf.size()));
  // For input PQ value at index i, EOTF gives a linear value;
  // applying OETF to that linear value should get back to ≈ input.
  // Skip low-end inputs (i < 500) — PQ's huge dynamic range at the
  // dark end means the EOTF output goes below 16-bit quantum
  // resolution, so a round-trip through the LUT can't recover it.
  for (const std::size_t i : {1000U, 2080U, 3000U, 4000U}) {
    const auto linear_q = eotf[i].red;  // u16
    // Find which OETF input would produce this linear value, by
    // inverse-lookup over the OETF LUT entries.
    const auto oetf_idx =
        static_cast<std::size_t>(std::round(static_cast<double>(linear_q) / 65535.0 * 4095.0));
    const auto recovered_q = oetf[oetf_idx].red;
    const auto expected_q =
        static_cast<std::uint16_t>(std::round(static_cast<double>(i) / 4095.0 * 65535.0));
    // ~200-count tolerance: two LUT discretisations stack (4096
    // entries quantizing input PQ → output linear → input PQ again),
    // and the PQ curve is steepest near the low end where this
    // matters most.
    EXPECT_NEAR(recovered_q, expected_q, 200) << "i=" << i;
  }
}

// ── HLG OETF^-1 ─────────────────────────────────────────────────────

TEST(ColorPipelineCurves, HlgOetfInverseEndpoints) {
  std::vector<drm_color_lut> lut(4096);
  drm::display::build_hlg_oetf_inverse_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  EXPECT_EQ(lut.front().red, 0U) << "HLG^-1(0) == 0";
  // BT.2100: HLG^-1(1) = (exp((1 - 0.55991073)/0.17883277) + 0.28466892)/12 ≈ 1.0
  EXPECT_EQ(lut.back().red, 0xFFFFU);
}

TEST(ColorPipelineCurves, HlgOetfInverseAtHalfMatchesSpec) {
  // At E' = 0.5 the spec says HLG^-1 = (0.5)^2 / 3 = 0.0833...
  std::vector<drm_color_lut> lut(4096);
  drm::display::build_hlg_oetf_inverse_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  const auto idx = static_cast<std::size_t>(std::round(0.5 * 4095.0));
  // 0.0833 * 65535 ≈ 5461.
  EXPECT_NEAR(lut[idx].red, 5461U, 20U);
}

TEST(ColorPipelineCurves, HlgOetfInverseMonotonic) {
  std::vector<drm_color_lut> lut(4096);
  drm::display::build_hlg_oetf_inverse_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  for (std::size_t i = 1; i < lut.size(); ++i) {
    EXPECT_GE(lut[i].red, lut[i - 1].red);
  }
}

// ── CTM builders ────────────────────────────────────────────────────

TEST(ColorPipelineCurves, IdentityCtmDiagonalIsOne) {
  const auto ctm = drm::display::build_identity_ctm();
  EXPECT_EQ(ctm.matrix[0], k_s31_32_one);
  EXPECT_EQ(ctm.matrix[4], k_s31_32_one);
  EXPECT_EQ(ctm.matrix[8], k_s31_32_one);
  EXPECT_EQ(ctm.matrix[1], 0U);
  EXPECT_EQ(ctm.matrix[2], 0U);
  EXPECT_EQ(ctm.matrix[3], 0U);
  EXPECT_EQ(ctm.matrix[5], 0U);
  EXPECT_EQ(ctm.matrix[6], 0U);
  EXPECT_EQ(ctm.matrix[7], 0U);
}

TEST(ColorPipelineCurves, Bt2020ToBt709MatrixRoundTrips) {
  // Decode each entry; verify against ITU-R BT.2087 published values.
  const auto ctm = drm::display::build_bt2020_to_bt709_ctm();
  EXPECT_NEAR(drm::display::decode_s31_32(ctm.matrix[0]), 1.6605, 1e-4);
  EXPECT_NEAR(drm::display::decode_s31_32(ctm.matrix[1]), -0.5876, 1e-4);
  EXPECT_NEAR(drm::display::decode_s31_32(ctm.matrix[2]), -0.0728, 1e-4);
  EXPECT_NEAR(drm::display::decode_s31_32(ctm.matrix[3]), -0.1246, 1e-4);
  EXPECT_NEAR(drm::display::decode_s31_32(ctm.matrix[4]), 1.1329, 1e-4);
  EXPECT_NEAR(drm::display::decode_s31_32(ctm.matrix[5]), -0.0083, 1e-4);
  EXPECT_NEAR(drm::display::decode_s31_32(ctm.matrix[6]), -0.0182, 1e-4);
  EXPECT_NEAR(drm::display::decode_s31_32(ctm.matrix[7]), -0.1006, 1e-4);
  EXPECT_NEAR(drm::display::decode_s31_32(ctm.matrix[8]), 1.1187, 1e-4);
}

TEST(ColorPipelineCurves, Bt709ToBt2020IsApproximateInverse) {
  // Composing the two matrices should approximate identity (the
  // published values are 4-digit rounded; product diverges from
  // exact identity by a few thousandths).
  const auto a = drm::display::build_bt2020_to_bt709_ctm();
  const auto b = drm::display::build_bt709_to_bt2020_ctm();
  // C = A * B, expect ≈ identity.
  std::array<double, 9> aa{};
  std::array<double, 9> bb{};
  std::array<double, 9> cc{};
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
  for (std::size_t i = 0; i < 9; ++i) {
    aa[i] = drm::display::decode_s31_32(a.matrix[i]);
    bb[i] = drm::display::decode_s31_32(b.matrix[i]);
  }
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      double sum = 0;
      for (std::size_t k = 0; k < 3; ++k) {
        sum += aa[(r * 3) + k] * bb[(k * 3) + c];
      }
      cc[(r * 3) + c] = sum;
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  // Diagonal close to 1, off-diagonal close to 0.
  EXPECT_NEAR(cc[0], 1.0, 0.01);
  EXPECT_NEAR(cc[4], 1.0, 0.01);
  EXPECT_NEAR(cc[8], 1.0, 0.01);
  EXPECT_NEAR(cc[1], 0.0, 0.01);
  EXPECT_NEAR(cc[2], 0.0, 0.01);
  EXPECT_NEAR(cc[3], 0.0, 0.01);
  EXPECT_NEAR(cc[5], 0.0, 0.01);
  EXPECT_NEAR(cc[6], 0.0, 0.01);
  EXPECT_NEAR(cc[7], 0.0, 0.01);
}

}  // namespace
