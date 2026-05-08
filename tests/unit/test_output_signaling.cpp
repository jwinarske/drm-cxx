// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "scene/display_params.hpp"
#include "scene/output_signaling.hpp"

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/connector_capabilities.hpp>
#include <drm-cxx/display/connector_info.hpp>
#include <drm-cxx/display/hdr_metadata.hpp>

#include <array>
#include <gtest/gtest.h>

namespace {

using drm::scene::ColorPrimaries;
using drm::scene::DisplayParams;

DisplayParams with_primaries(ColorPrimaries cp) {
  DisplayParams dp;
  dp.color_primaries = cp;
  return dp;
}

// Sentinel that no real test expects to match.
constexpr ColorPrimaries k_sentinel = ColorPrimaries::Bt709;

TEST(OutputSignallingTest, Bt709PrimariesAreCanonicalSrgb) {
  const auto info = drm::scene::color_primaries_to_colorimetry(ColorPrimaries::Bt709);
  EXPECT_TRUE(info.has_primaries);
  EXPECT_TRUE(info.has_default_white);
  EXPECT_FLOAT_EQ(info.red.x, 0.640F);
  EXPECT_FLOAT_EQ(info.red.y, 0.330F);
  EXPECT_FLOAT_EQ(info.green.x, 0.300F);
  EXPECT_FLOAT_EQ(info.green.y, 0.600F);
  EXPECT_FLOAT_EQ(info.blue.x, 0.150F);
  EXPECT_FLOAT_EQ(info.blue.y, 0.060F);
  EXPECT_FLOAT_EQ(info.white.x, 0.3127F);
  EXPECT_FLOAT_EQ(info.white.y, 0.3290F);
}

TEST(OutputSignallingTest, Bt2020PrimariesMatchRec2100Spec) {
  const auto info = drm::scene::color_primaries_to_colorimetry(ColorPrimaries::Bt2020);
  EXPECT_FLOAT_EQ(info.red.x, 0.708F);
  EXPECT_FLOAT_EQ(info.red.y, 0.292F);
  EXPECT_FLOAT_EQ(info.green.x, 0.170F);
  EXPECT_FLOAT_EQ(info.green.y, 0.797F);
  EXPECT_FLOAT_EQ(info.blue.x, 0.131F);
  EXPECT_FLOAT_EQ(info.blue.y, 0.046F);
  EXPECT_FLOAT_EQ(info.white.x, 0.3127F);
  EXPECT_FLOAT_EQ(info.white.y, 0.3290F);
}

TEST(OutputSignallingTest, DciP3D65PrimariesMatchSmpteEg432) {
  const auto info = drm::scene::color_primaries_to_colorimetry(ColorPrimaries::DciP3);
  EXPECT_FLOAT_EQ(info.red.x, 0.680F);
  EXPECT_FLOAT_EQ(info.red.y, 0.320F);
  EXPECT_FLOAT_EQ(info.green.x, 0.265F);
  EXPECT_FLOAT_EQ(info.green.y, 0.690F);
  EXPECT_FLOAT_EQ(info.blue.x, 0.150F);
  EXPECT_FLOAT_EQ(info.blue.y, 0.060F);
  EXPECT_FLOAT_EQ(info.white.x, 0.3127F);
  EXPECT_FLOAT_EQ(info.white.y, 0.3290F);
}

TEST(OutputSignallingTest, AdobeRgbPrimariesMatch1998Spec) {
  const auto info = drm::scene::color_primaries_to_colorimetry(ColorPrimaries::AdobeRgb);
  EXPECT_FLOAT_EQ(info.red.x, 0.640F);
  EXPECT_FLOAT_EQ(info.red.y, 0.330F);
  EXPECT_FLOAT_EQ(info.green.x, 0.210F);
  EXPECT_FLOAT_EQ(info.green.y, 0.710F);
  EXPECT_FLOAT_EQ(info.blue.x, 0.150F);
  EXPECT_FLOAT_EQ(info.blue.y, 0.060F);
  EXPECT_FLOAT_EQ(info.white.x, 0.3127F);
  EXPECT_FLOAT_EQ(info.white.y, 0.3290F);
}

TEST(OutputSignallingTest, WidestGamutEmptySpanReturnsNullopt) {
  const drm::span<const DisplayParams* const> empty{};
  EXPECT_FALSE(drm::scene::widest_gamut(empty).has_value());
}

TEST(OutputSignallingTest, WidestGamutAllUnsetReturnsNullopt) {
  const DisplayParams a;
  const DisplayParams b;
  const std::array<const DisplayParams*, 2> layers{&a, &b};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  EXPECT_FALSE(drm::scene::widest_gamut(span).has_value());
}

TEST(OutputSignallingTest, WidestGamutSinglePassthrough) {
  const auto dp = with_primaries(ColorPrimaries::DciP3);
  const std::array<const DisplayParams*, 1> layers{&dp};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  EXPECT_EQ(drm::scene::widest_gamut(span).value_or(k_sentinel), ColorPrimaries::DciP3);
}

TEST(OutputSignallingTest, WidestGamutBt2020WinsOverDciP3) {
  const auto a = with_primaries(ColorPrimaries::DciP3);
  const auto b = with_primaries(ColorPrimaries::Bt2020);
  const std::array<const DisplayParams*, 2> layers{&a, &b};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  EXPECT_EQ(drm::scene::widest_gamut(span).value_or(k_sentinel), ColorPrimaries::Bt2020);
}

TEST(OutputSignallingTest, WidestGamutDciP3WinsOverAdobeRgb) {
  const auto a = with_primaries(ColorPrimaries::AdobeRgb);
  const auto b = with_primaries(ColorPrimaries::DciP3);
  const std::array<const DisplayParams*, 2> layers{&a, &b};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  EXPECT_EQ(drm::scene::widest_gamut(span).value_or(k_sentinel), ColorPrimaries::DciP3);
}

TEST(OutputSignallingTest, WidestGamutAdobeRgbWinsOverBt709) {
  const auto a = with_primaries(ColorPrimaries::Bt709);
  const auto b = with_primaries(ColorPrimaries::AdobeRgb);
  const std::array<const DisplayParams*, 2> layers{&a, &b};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  EXPECT_EQ(drm::scene::widest_gamut(span).value_or(k_sentinel), ColorPrimaries::AdobeRgb);
}

TEST(OutputSignallingTest, WidestGamutMixedSetSelectsWidest) {
  const auto a = with_primaries(ColorPrimaries::Bt709);
  const DisplayParams b;  // unset — skipped
  const auto c = with_primaries(ColorPrimaries::DciP3);
  const auto d = with_primaries(ColorPrimaries::Bt2020);
  const auto e = with_primaries(ColorPrimaries::AdobeRgb);
  const std::array<const DisplayParams*, 5> layers{&a, &b, &c, &d, &e};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  EXPECT_EQ(drm::scene::widest_gamut(span).value_or(k_sentinel), ColorPrimaries::Bt2020);
}

TEST(OutputSignallingTest, WidestGamutTolerantOfNullEntries) {
  // Caller may pass a sparse list (e.g. some slots empty in the
  // scene's per-frame scratch buffer).
  const auto a = with_primaries(ColorPrimaries::Bt2020);
  const std::array<const DisplayParams*, 3> layers{nullptr, &a, nullptr};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  EXPECT_EQ(drm::scene::widest_gamut(span).value_or(k_sentinel), ColorPrimaries::Bt2020);
}

TEST(DisplayParamsTest, ColorPrimariesAndSourceEotfDefaultUnset) {
  const DisplayParams dp;
  EXPECT_FALSE(dp.color_primaries.has_value());
  EXPECT_FALSE(dp.source_eotf.has_value());
}

// ── color_primaries_to_colorspace ───────────────────────────────────

TEST(OutputSignallingTest, Bt709MapsToColorspaceDefault) {
  // BT.709 → Default lets the driver pick its standard SDR signaling
  // rather than forcing BT709_YCC, which would force a YCC AVI flag
  // even on RGB sources.
  EXPECT_EQ(drm::scene::color_primaries_to_colorspace(ColorPrimaries::Bt709),
            drm::display::Colorspace::Default);
}

TEST(OutputSignallingTest, Bt2020MapsToColorspaceBt2020Rgb) {
  EXPECT_EQ(drm::scene::color_primaries_to_colorspace(ColorPrimaries::Bt2020),
            drm::display::Colorspace::Bt2020Rgb);
}

TEST(OutputSignallingTest, DciP3MapsToDciP3RgbD65) {
  EXPECT_EQ(drm::scene::color_primaries_to_colorspace(ColorPrimaries::DciP3),
            drm::display::Colorspace::DciP3RgbD65);
}

TEST(OutputSignallingTest, AdobeRgbMapsToOpRgb) {
  EXPECT_EQ(drm::scene::color_primaries_to_colorspace(ColorPrimaries::AdobeRgb),
            drm::display::Colorspace::OpRgb);
}

// ── derive_output_signaling ────────────────────────────────────────

DisplayParams with_eotf(drm::display::TransferFunction tf) {
  DisplayParams dp;
  dp.source_eotf = tf;
  return dp;
}

DisplayParams with_primaries_and_eotf(ColorPrimaries cp, drm::display::TransferFunction tf) {
  DisplayParams dp;
  dp.color_primaries = cp;
  dp.source_eotf = tf;
  return dp;
}

TEST(OutputSignallingTest, DeriveEmptyLayersReturnsAllNullopt) {
  const drm::span<const DisplayParams* const> empty{};
  const auto sig = drm::scene::derive_output_signaling(empty);
  EXPECT_FALSE(sig.colorspace.has_value());
  EXPECT_FALSE(sig.hdr_metadata.has_value());
}

TEST(OutputSignallingTest, DeriveAllSdrAllUnsetReturnsAllNullopt) {
  const DisplayParams a;
  const DisplayParams b;
  const std::array<const DisplayParams*, 2> layers{&a, &b};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  const auto sig = drm::scene::derive_output_signaling(span);
  EXPECT_FALSE(sig.colorspace.has_value());
  EXPECT_FALSE(sig.hdr_metadata.has_value());
}

TEST(OutputSignallingTest, DeriveBt709SdrLayerSetsDefaultColorspaceNoHdr) {
  const auto a = with_primaries_and_eotf(ColorPrimaries::Bt709,
                                         drm::display::TransferFunction::TraditionalGammaSdr);
  const std::array<const DisplayParams*, 1> layers{&a};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  const auto sig = drm::scene::derive_output_signaling(span);
  EXPECT_EQ(sig.colorspace.value_or(drm::display::Colorspace::Bt2020Rgb),
            drm::display::Colorspace::Default);
  EXPECT_FALSE(sig.hdr_metadata.has_value()) << "SDR EOTF must not trigger HDR signaling";
}

TEST(OutputSignallingTest, DeriveBt2020PqLayerProducesHdrSignalling) {
  const auto a = with_primaries_and_eotf(ColorPrimaries::Bt2020,
                                         drm::display::TransferFunction::SmpteSt2084Pq);
  const std::array<const DisplayParams*, 1> layers{&a};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  const auto sig = drm::scene::derive_output_signaling(span);
  EXPECT_EQ(sig.colorspace.value_or(drm::display::Colorspace::Default),
            drm::display::Colorspace::Bt2020Rgb);
  ASSERT_TRUE(sig.hdr_metadata.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
  const auto md = sig.hdr_metadata.value();
  EXPECT_EQ(md.eotf, drm::display::TransferFunction::SmpteSt2084Pq);
  // Mastering primaries seeded from the BT.2020 layer.
  EXPECT_FLOAT_EQ(md.display_primaries.red.x, 0.708F);
  EXPECT_FLOAT_EQ(md.display_primaries.green.y, 0.797F);
  // Luminance fields stay 0 — manual API supplies those.
  EXPECT_EQ(md.max_display_mastering_luminance, 0U);
  EXPECT_EQ(md.max_content_light_level, 0U);
}

TEST(OutputSignallingTest, DeriveMixedSdrAndHdrPicksWidestGamutAndHdr) {
  // A BT.709 SDR layer + a BT.2020 PQ HDR layer → connector goes
  // BT.2020, HDR signaling on. Plan §5.2 baseline case.
  const auto sdr = with_primaries_and_eotf(ColorPrimaries::Bt709,
                                           drm::display::TransferFunction::TraditionalGammaSdr);
  const auto hdr = with_primaries_and_eotf(ColorPrimaries::Bt2020,
                                           drm::display::TransferFunction::SmpteSt2084Pq);
  const std::array<const DisplayParams*, 2> layers{&sdr, &hdr};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  const auto sig = drm::scene::derive_output_signaling(span);
  EXPECT_EQ(sig.colorspace.value_or(drm::display::Colorspace::Default),
            drm::display::Colorspace::Bt2020Rgb);
  ASSERT_TRUE(sig.hdr_metadata.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(sig.hdr_metadata.value().eotf, drm::display::TransferFunction::SmpteSt2084Pq);
}

TEST(OutputSignallingTest, DerivePqWinsOverHlgWhenBothPresent) {
  const auto pq = with_eotf(drm::display::TransferFunction::SmpteSt2084Pq);
  const auto hlg = with_eotf(drm::display::TransferFunction::Bt2100Hlg);
  // Order PQ first, HLG second — PQ should win.
  {
    const std::array<const DisplayParams*, 2> layers{&pq, &hlg};
    const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
    const auto sig = drm::scene::derive_output_signaling(span);
    ASSERT_TRUE(sig.hdr_metadata.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    EXPECT_EQ(sig.hdr_metadata.value().eotf, drm::display::TransferFunction::SmpteSt2084Pq);
  }
  // Reverse order — PQ still wins.
  {
    const std::array<const DisplayParams*, 2> layers{&hlg, &pq};
    const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
    const auto sig = drm::scene::derive_output_signaling(span);
    ASSERT_TRUE(sig.hdr_metadata.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    EXPECT_EQ(sig.hdr_metadata.value().eotf, drm::display::TransferFunction::SmpteSt2084Pq);
  }
}

TEST(OutputSignallingTest, DeriveHlgOnlyProducesHlgMetadata) {
  const auto a = with_eotf(drm::display::TransferFunction::Bt2100Hlg);
  const std::array<const DisplayParams*, 1> layers{&a};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  const auto sig = drm::scene::derive_output_signaling(span);
  ASSERT_TRUE(sig.hdr_metadata.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(sig.hdr_metadata.value().eotf, drm::display::TransferFunction::Bt2100Hlg);
}

TEST(OutputSignallingTest, DeriveHdrLayerWithoutPrimariesDefaultsToBt2020Mastering) {
  // source_eotf is HDR but color_primaries is unset — mastering
  // primaries should default to BT.2020 per the design.
  const auto a = with_eotf(drm::display::TransferFunction::SmpteSt2084Pq);
  const std::array<const DisplayParams*, 1> layers{&a};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  const auto sig = drm::scene::derive_output_signaling(span);
  EXPECT_FALSE(sig.colorspace.has_value()) << "no color_primaries → no Colorspace write";
  ASSERT_TRUE(sig.hdr_metadata.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_FLOAT_EQ(sig.hdr_metadata.value().display_primaries.red.x, 0.708F);  // BT.2020 red x
}

TEST(OutputSignallingTest, DeriveTraditionalGammaHdrIsNotTreatedAsHdr) {
  // TraditionalGammaHdr is the rare "HDR10 over a BT.1886 path"
  // case the design documents but doesn't auto-trigger HDR
  // signaling — only PQ / HLG do.
  const auto a = with_eotf(drm::display::TransferFunction::TraditionalGammaHdr);
  const std::array<const DisplayParams*, 1> layers{&a};
  const drm::span<const DisplayParams* const> span{layers.data(), layers.size()};
  const auto sig = drm::scene::derive_output_signaling(span);
  EXPECT_FALSE(sig.hdr_metadata.has_value());
}

}  // namespace
