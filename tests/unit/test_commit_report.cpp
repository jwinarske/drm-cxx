// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for CommitReport's by-handle placement lookups — placement_of()
// and the was_composited() convenience a zero-copy producer uses to detect a
// a demotion to the composition canvas. Pure data; no device.

#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/layer_handle.hpp>

#include <cstdint>
#include <gtest/gtest.h>

namespace {

using drm::scene::CommitReport;
using drm::scene::LayerHandle;
using drm::scene::LayerPlacement;

LayerHandle h(std::uint32_t id, std::uint32_t gen = 1) {
  return LayerHandle{id, gen};
}

CommitReport report_with_placements() {
  CommitReport r;
  r.placements.push_back({h(1), LayerPlacement::AssignedToPlane, 31});
  r.placements.push_back({h(2), LayerPlacement::Composited, 42});
  r.placements.push_back({h(3), LayerPlacement::Unassigned, 0});
  return r;
}

}  // namespace

TEST(CommitReportPlacement, PlacementOfFindsEachOutcome) {
  const auto r = report_with_placements();

  const auto a = r.placement_of(h(1));
  ASSERT_TRUE(a.has_value());
  if (a.has_value()) {  // guard satisfies bugprone-unchecked-optional-access
    EXPECT_EQ(a->placement, LayerPlacement::AssignedToPlane);
    EXPECT_EQ(a->plane_id, 31U);
  }

  const auto c = r.placement_of(h(2));
  ASSERT_TRUE(c.has_value());
  if (c.has_value()) {
    EXPECT_EQ(c->placement, LayerPlacement::Composited);
    EXPECT_EQ(c->plane_id, 42U);
  }

  const auto u = r.placement_of(h(3));
  ASSERT_TRUE(u.has_value());
  if (u.has_value()) {
    EXPECT_EQ(u->placement, LayerPlacement::Unassigned);
  }
}

TEST(CommitReportPlacement, PlacementOfMissReturnsNullopt) {
  const auto r = report_with_placements();
  EXPECT_FALSE(r.placement_of(h(4)).has_value());               // unknown id
  EXPECT_FALSE(r.placement_of(h(1, 2)).has_value());            // right id, stale generation
  EXPECT_FALSE(CommitReport{}.placement_of(h(1)).has_value());  // empty report
}

TEST(CommitReportPlacement, WasCompositedOnlyForCompositedLayer) {
  const auto r = report_with_placements();
  EXPECT_FALSE(r.was_composited(h(1)));  // hardware plane
  EXPECT_TRUE(r.was_composited(h(2)));   // demoted to canvas
  EXPECT_FALSE(r.was_composited(h(3)));  // dropped, not composited
  EXPECT_FALSE(r.was_composited(h(9)));  // absent
}
