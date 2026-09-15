// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "planes/plane_registry.hpp"

#include <cstdint>
#include <gtest/gtest.h>

namespace {

constexpr std::uint64_t k_full = 0xFFFFU;

// A plane advertising the full 16-bit range needs no adjustment at all.
TEST(PlaneAlphaTest, FullRangeIsIdentity) {
  EXPECT_EQ(drm::planes::rescale_alpha(0U, k_full), 0U);
  EXPECT_EQ(drm::planes::rescale_alpha(0x4000U, k_full), 0x4000U);
  EXPECT_EQ(drm::planes::rescale_alpha(0x8000U, k_full), 0x8000U);
  EXPECT_EQ(drm::planes::rescale_alpha(k_full, k_full), k_full);
}

// Full opacity must land exactly on the advertised maximum. Anything less and
// a nominally opaque layer scans out slightly transparent; anything more and
// the kernel rejects the commit.
TEST(PlaneAlphaTest, FullOpacityMapsOntoAdvertisedMaximum) {
  EXPECT_EQ(drm::planes::rescale_alpha(k_full, 255U), 255U);
  EXPECT_EQ(drm::planes::rescale_alpha(k_full, 1023U), 1023U);
  EXPECT_EQ(drm::planes::rescale_alpha(k_full, 1U), 1U);
}

// The reason this rescales instead of clamping. std::min would return the
// advertised maximum for every one of these, turning a half-faded layer
// opaque -- a silent visual bug on any plane with 8-bit alpha.
TEST(PlaneAlphaTest, PartialAlphaKeepsItsFraction) {
  EXPECT_EQ(drm::planes::rescale_alpha(0x8000U, 255U), 128U);
  EXPECT_EQ(drm::planes::rescale_alpha(0x4000U, 255U), 64U);
  EXPECT_EQ(drm::planes::rescale_alpha(0x8000U, 1023U), 512U);
  EXPECT_EQ(drm::planes::rescale_alpha(0x4000U, 1023U), 256U);
}

// Fully transparent must stay fully transparent at every range: rounding that
// lifted 0 off the floor would make a hidden layer faintly visible.
TEST(PlaneAlphaTest, ZeroStaysZero) {
  EXPECT_EQ(drm::planes::rescale_alpha(0U, 255U), 0U);
  EXPECT_EQ(drm::planes::rescale_alpha(0U, 1023U), 0U);
  EXPECT_EQ(drm::planes::rescale_alpha(0U, 1U), 0U);
}

// A value above 16-bit full scale is caller error, but it must not scale past
// the plane's maximum and take the whole atomic commit down with it.
TEST(PlaneAlphaTest, OversizedInputSaturatesRatherThanOverflowing) {
  EXPECT_EQ(drm::planes::rescale_alpha(0x1'0000U, 255U), 255U);
  EXPECT_EQ(drm::planes::rescale_alpha(0xFFFF'FFFFU, 255U), 255U);
  EXPECT_EQ(drm::planes::rescale_alpha(0xFFFF'FFFFU, k_full), k_full);
}

// Monotonic across the whole input range: a caller raising alpha must never
// see the plane value drop.
TEST(PlaneAlphaTest, MonotonicNonDecreasing) {
  for (const std::uint64_t alpha_max : {1U, 15U, 255U, 1023U, 4095U}) {
    std::uint64_t previous = 0U;
    for (std::uint64_t v = 0U; v <= k_full; v += 97U) {
      const std::uint64_t got = drm::planes::rescale_alpha(v, alpha_max);
      EXPECT_GE(got, previous) << "alpha_max=" << alpha_max << " value=" << v;
      EXPECT_LE(got, alpha_max) << "alpha_max=" << alpha_max << " value=" << v;
      previous = got;
    }
  }
}

// The property range is the driver's to declare, so nothing may assume 255.
// A plane advertising a larger-but-not-full range is rescaled to its own
// maximum, not to some other implementation's.
TEST(PlaneAlphaTest, RespectsEachPlanesOwnAdvertisedRange) {
  EXPECT_EQ(drm::planes::rescale_alpha(k_full, 0x7FFFU), 0x7FFFU);
  EXPECT_EQ(drm::planes::rescale_alpha(0x8000U, 0x7FFFU), 0x4000U);
  // Degenerate but legal: a plane that only supports on/off.
  EXPECT_EQ(drm::planes::rescale_alpha(k_full / 2U, 1U), 0U);
  EXPECT_EQ(drm::planes::rescale_alpha(k_full, 1U), 1U);
}

}  // namespace
