// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "display/tone_mapper.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <string_view>

namespace {

using drm::display::ToneMapCurve;
using drm::display::ToneMapper;

constexpr std::uint64_t pack(std::uint16_t r, std::uint16_t g, std::uint16_t b,
                             std::uint16_t a = 0xFFFFU) noexcept {
  return static_cast<std::uint64_t>(r) | (static_cast<std::uint64_t>(g) << 16U) |
         (static_cast<std::uint64_t>(b) << 32U) | (static_cast<std::uint64_t>(a) << 48U);
}

constexpr std::uint16_t unpack_r(std::uint64_t p) noexcept {
  return static_cast<std::uint16_t>(p & 0xFFFFU);
}
constexpr std::uint16_t unpack_g(std::uint64_t p) noexcept {
  return static_cast<std::uint16_t>((p >> 16U) & 0xFFFFU);
}
constexpr std::uint16_t unpack_b(std::uint64_t p) noexcept {
  return static_cast<std::uint16_t>((p >> 32U) & 0xFFFFU);
}
constexpr std::uint16_t unpack_a(std::uint64_t p) noexcept {
  return static_cast<std::uint16_t>((p >> 48U) & 0xFFFFU);
}

// ── Construction / dispatch ─────────────────────────────────────────

TEST(ToneMapper, FactoriesRecordDirection) {
  EXPECT_EQ(ToneMapper::bt709_to_bt2020_pq().direction(), ToneMapper::Direction::Bt709ToBt2020Pq);
  EXPECT_EQ(ToneMapper::bt2020_pq_to_bt709().direction(), ToneMapper::Direction::Bt2020PqToBt709);
  EXPECT_EQ(ToneMapper::hlg_to_bt709().direction(), ToneMapper::Direction::HlgToBt709);
}

TEST(ToneMapper, BlackPixelStaysBlack) {
  // Black goes to black through every direction; a useful sanity
  // check independent of the curve / target_max_nits.
  const auto bk = pack(0, 0, 0);
  EXPECT_EQ(unpack_r(ToneMapper::bt709_to_bt2020_pq()(bk)), 0U);
  EXPECT_EQ(unpack_g(ToneMapper::bt709_to_bt2020_pq()(bk)), 0U);
  EXPECT_EQ(unpack_b(ToneMapper::bt709_to_bt2020_pq()(bk)), 0U);
  EXPECT_EQ(unpack_r(ToneMapper::bt2020_pq_to_bt709()(bk)), 0U);
  EXPECT_EQ(unpack_g(ToneMapper::bt2020_pq_to_bt709()(bk)), 0U);
  EXPECT_EQ(unpack_b(ToneMapper::bt2020_pq_to_bt709()(bk)), 0U);
}

TEST(ToneMapper, AlphaPassesThroughUnchanged) {
  const auto in = pack(0x4000U, 0x8000U, 0xC000U, 0xABCDU);
  const auto out = ToneMapper::bt2020_pq_to_bt709()(in);
  EXPECT_EQ(unpack_a(out), 0xABCDU);
}

// ── BT.709 → BT.2020 PQ ─────────────────────────────────────────────

TEST(ToneMapper, Bt709ToBt2020PqWhiteAt100NitsLandsBelow0p51) {
  // Full white SDR (1.0) with sdr_white_nits=100 should land at
  // PQ value ≈ 0.508 (the published encoded value for 100 cd/m²).
  const auto in = pack(0xFFFFU, 0xFFFFU, 0xFFFFU);
  const auto out = ToneMapper::bt709_to_bt2020_pq(100.0F)(in);
  // 0.508 * 65535 ≈ 33291.
  EXPECT_NEAR(unpack_r(out), 33291U, 200U);
  EXPECT_NEAR(unpack_g(out), 33291U, 200U);
  EXPECT_NEAR(unpack_b(out), 33291U, 200U);
}

TEST(ToneMapper, Bt709ToBt2020PqHigherWhiteShiftsHigher) {
  const auto white = pack(0xFFFFU, 0xFFFFU, 0xFFFFU);
  const auto at_100 = ToneMapper::bt709_to_bt2020_pq(100.0F)(white);
  const auto at_300 = ToneMapper::bt709_to_bt2020_pq(300.0F)(white);
  EXPECT_GT(unpack_r(at_300), unpack_r(at_100))
      << "Higher SDR-white nits should encode brighter on PQ";
}

// ── BT.2020 PQ → BT.709 ─────────────────────────────────────────────

TEST(ToneMapper, Bt2020PqToBt709Mid_GrayPqMapsToReasonableSdr) {
  // PQ encoded value 0.508 (≈ 100 cd/m²) → SDR full-white (1.0)
  // when target_max_nits = 100 with Reinhard. Reinhard's curve at
  // input == 1.0 outputs 0.5, then BT.1886 OETF lifts it to
  // ≈ 0.5^(1/2.4) ≈ 0.758. Quantized: ≈ 0.758 * 65535 ≈ 49680.
  const auto pq_100 = pack(33291U, 33291U, 33291U);
  const auto out = ToneMapper::bt2020_pq_to_bt709(100.0F)(pq_100);
  EXPECT_NEAR(unpack_r(out), 49680U, 1500U);
}

TEST(ToneMapper, Bt2020PqToBt709NoneCurveClipsAt100Nits) {
  // With curve=None and target=100 nits, anything brighter than
  // 100 cd/m² clips at 1.0 in linear. PQ value 0.7518 ≈ 1000 nits
  // — should clip to full white SDR.
  const auto pq_1000 = pack(49259U, 49259U, 49259U);  // 0.7518 * 65535
  const auto out = ToneMapper::bt2020_pq_to_bt709(100.0F, ToneMapCurve::None)(pq_1000);
  EXPECT_EQ(unpack_r(out), 0xFFFFU);
}

TEST(ToneMapper, Bt2020PqToBt709ReinhardCompressesHighlights) {
  // Reinhard maps anything in [0, ∞] to [0, 1] smoothly. A 1000-nit
  // signal with target=100 nits should land below SDR full-white.
  const auto pq_1000 = pack(49259U, 49259U, 49259U);
  const auto reinhard = ToneMapper::bt2020_pq_to_bt709(100.0F, ToneMapCurve::Reinhard)(pq_1000);
  EXPECT_LT(unpack_r(reinhard), 0xFFFFU) << "Reinhard must not clip at full white";
  EXPECT_GT(unpack_r(reinhard), 50000U) << "Should still be bright";
}

TEST(ToneMapper, Bt2020PqToBt709HableProducesDifferentMidtone) {
  // Reinhard and Hable should disagree on where mid-tone lands —
  // Hable's S-shape darkens slightly and brightens highlights.
  const auto pq_500 = pack(40000U, 40000U, 40000U);
  const auto reinhard = ToneMapper::bt2020_pq_to_bt709(100.0F, ToneMapCurve::Reinhard)(pq_500);
  const auto hable = ToneMapper::bt2020_pq_to_bt709(100.0F, ToneMapCurve::Hable)(pq_500);
  EXPECT_NE(unpack_r(reinhard), unpack_r(hable))
      << "Reinhard / Hable diverge; if they ever produce identical output the test is wrong";
}

// ── HLG → BT.709 ────────────────────────────────────────────────────

TEST(ToneMapper, HlgToBt709WhitePointBelowFullWhite) {
  // HLG full-white encoded → SDR somewhere near full-white but
  // not clipping. The exact value depends on OOTF + Reinhard, so
  // we just bound it loosely.
  const auto white = pack(0xFFFFU, 0xFFFFU, 0xFFFFU);
  const auto out = ToneMapper::hlg_to_bt709(100.0F)(white);
  EXPECT_LT(unpack_r(out), 0xFFFFU);
  EXPECT_GT(unpack_r(out), 32000U) << "HLG full-white should render bright SDR";
}

TEST(ToneMapper, HlgBlackStaysBlack) {
  const auto bk = pack(0, 0, 0);
  const auto out = ToneMapper::hlg_to_bt709()(bk);
  EXPECT_EQ(unpack_r(out), 0U);
  EXPECT_EQ(unpack_g(out), 0U);
  EXPECT_EQ(unpack_b(out), 0U);
}

// ── Round-trip ──────────────────────────────────────────────────────

TEST(ToneMapper, RoundTripSdrThroughHdrMidGrayApproximate) {
  // Mid-gray through SDR → HDR PQ → SDR (no tone-map curve) should
  // return close to the input, modulo the color-space round-trip
  // which is lossy at the matrix step.
  const auto src = pack(32768U, 32768U, 32768U);
  const auto hdr = ToneMapper::bt709_to_bt2020_pq(100.0F)(src);
  const auto sdr = ToneMapper::bt2020_pq_to_bt709(100.0F, ToneMapCurve::None)(hdr);
  // The full pipeline (sRGB EOTF → matrix → PQ OETF → PQ EOTF →
  // matrix → BT.1886 OETF) accumulates error from BT.709/BT.2020
  // matrix non-orthogonality + the BT.1886 vs sRGB OETF mismatch.
  // Mid-gray lands in a different SDR encoded value than the input;
  // accept ~10% drift as the cost of the round-trip.
  EXPECT_NEAR(unpack_r(sdr), unpack_r(src), 8000U);
}

TEST(ToneMapper, NeutralInputProducesNeutralOutput) {
  // Equal R/G/B should stay neutral (no chromatic shift) for
  // mid-gray through any mapper. Bigger tolerance because the
  // BT.2020/BT.709 matrix isn't perfectly diagonal so even a
  // gray ramp picks up a tiny amount of ΔRGB; what we really want
  // is that the channels track each other.
  const auto src = pack(32768U, 32768U, 32768U);
  for (const auto* m : {"sdr_to_hdr", "hdr_to_sdr", "hlg_to_sdr"}) {
    std::uint64_t out = 0;
    if (std::string_view(m) == "sdr_to_hdr") {
      out = ToneMapper::bt709_to_bt2020_pq()(src);
    } else if (std::string_view(m) == "hdr_to_sdr") {
      out = ToneMapper::bt2020_pq_to_bt709()(src);
    } else {
      out = ToneMapper::hlg_to_bt709()(src);
    }
    EXPECT_NEAR(unpack_r(out), unpack_g(out), 200U) << m;
    EXPECT_NEAR(unpack_g(out), unpack_b(out), 200U) << m;
  }
}

// ── Pure color preservation (R/G/B independence sanity) ────────────

TEST(ToneMapper, PrimariesStayPrimaryThroughBt709ToBt2020Pq) {
  // Pure red SDR should be brightest in red channel after the
  // round-trip. Matrix coupling makes other channels nonzero but
  // they should be much smaller.
  const auto red = pack(0xFFFFU, 0, 0);
  const auto out = ToneMapper::bt709_to_bt2020_pq(100.0F)(red);
  EXPECT_GT(unpack_r(out), unpack_g(out));
  EXPECT_GT(unpack_r(out), unpack_b(out));
}

}  // namespace
