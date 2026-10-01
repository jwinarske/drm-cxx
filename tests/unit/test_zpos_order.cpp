// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "planes/plane_registry.hpp"
#include "planes/zpos_order.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <optional>

using drm::planes::DRMPlaneType;
using drm::planes::PlaneCapabilities;
using drm::planes::detail::effective_zpos;
using drm::planes::detail::stacking_consistent;
using drm::planes::detail::zpos_fixed;

namespace {

PlaneCapabilities plane(std::uint32_t id, DRMPlaneType type, std::optional<std::uint64_t> zmin,
                        std::optional<std::uint64_t> zmax) {
  PlaneCapabilities p;
  p.id = id;
  p.type = type;
  p.zpos_min = zmin;
  p.zpos_max = zmax;
  return p;
}

// i.MX LCDIF / vc4: PRIMARY pinned at 0.
const PlaneCapabilities k_primary_0 = plane(31, DRMPlaneType::PRIMARY, 0, 0);
// amdgpu DC: PRIMARY pinned at 2.
const PlaneCapabilities k_primary_2 = plane(40, DRMPlaneType::PRIMARY, 2, 2);
// A mutable overlay.
const PlaneCapabilities k_overlay = plane(50, DRMPlaneType::OVERLAY, 0, 255);
// A plane with no zpos property at all (e.g. Tegra display).
const PlaneCapabilities k_no_zpos = plane(60, DRMPlaneType::OVERLAY, std::nullopt, std::nullopt);

}  // namespace

TEST(ZposOrder, FixedOnlyWhenMinEqualsMax) {
  EXPECT_TRUE(zpos_fixed(k_primary_0));
  EXPECT_TRUE(zpos_fixed(k_primary_2));
  EXPECT_FALSE(zpos_fixed(k_overlay));
  EXPECT_FALSE(zpos_fixed(k_no_zpos));
}

TEST(ZposOrder, EffectiveZposIsSlotOnFixedPlanesElseRequested) {
  EXPECT_EQ(effective_zpos(k_primary_2, 7), 2U);
  EXPECT_EQ(effective_zpos(k_overlay, 7), 7U);
  EXPECT_EQ(effective_zpos(k_no_zpos, 7), 7U);
  EXPECT_EQ(effective_zpos(k_overlay, std::nullopt), std::nullopt);
}

// The bottom layer may sit on the fixed PRIMARY whatever its zpos as long as
// everything above it lands higher — the amdgpu "zpos <= 2 collides with the
// PRIMARY slot" case, and the single-plane i.MX case.
TEST(ZposOrder, BottomLayerOnFixedPrimaryKeepsOrder) {
  EXPECT_TRUE(stacking_consistent(k_primary_2, 0, k_overlay, 3));
  EXPECT_TRUE(stacking_consistent(k_primary_0, 1, k_overlay, 5));
}

// The top layer on the fixed PRIMARY while a lower layer sits on an overlay
// above the slot inverts the stack — TEST accepts it, so this must refuse it.
TEST(ZposOrder, InvertedStackRejected) {
  EXPECT_FALSE(stacking_consistent(k_primary_2, 5, k_overlay, 3));
  EXPECT_FALSE(stacking_consistent(k_overlay, 3, k_primary_2, 5));
}

// Different requested zpos landing on the same effective slot is ambiguous
// (the kernel breaks the tie by plane id).
TEST(ZposOrder, EqualEffectiveSlotForDifferentRequestRejected) {
  EXPECT_FALSE(stacking_consistent(k_primary_2, 5, k_overlay, 2));
}

TEST(ZposOrder, UnorderedRequestsAlwaysConsistent) {
  EXPECT_TRUE(stacking_consistent(k_primary_2, std::nullopt, k_overlay, 3));
  EXPECT_TRUE(stacking_consistent(k_primary_2, 4, k_overlay, 4));
  EXPECT_TRUE(stacking_consistent(k_no_zpos, 9, k_primary_0, 1));
}

// Under the pre-existing gate every placement had effective == requested
// (mutable planes are written; fixed planes only admitted layer.zpos == slot),
// so the new rule must accept all of those unchanged.
TEST(ZposOrder, PreviouslyLegalAssignmentsUnaffected) {
  EXPECT_TRUE(stacking_consistent(k_overlay, 1, k_overlay, 9));
  EXPECT_TRUE(stacking_consistent(k_overlay, 9, k_overlay, 1));
  EXPECT_TRUE(stacking_consistent(k_primary_2, 2, k_overlay, 5));
  EXPECT_TRUE(stacking_consistent(k_primary_0, 0, k_overlay, 1));
}
