// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "scene/display_params.hpp"
#include "scene/output_signaling.hpp"

#include <drm-cxx/detail/span.hpp>

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

}  // namespace
