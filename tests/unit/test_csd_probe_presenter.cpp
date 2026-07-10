// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include <drm-cxx/csd/presenter.hpp>
#include <drm-cxx/csd/probe_presenter.hpp>

#include <gtest/gtest.h>

namespace {

using drm::csd::choose_presenter_tier;
using drm::csd::Tier;

}  // namespace

// Plane wins only when every desired window can get its own overlay.
TEST(ChoosePresenterTier, PlaneWhenAllDesiredReservable) {
  EXPECT_EQ(choose_presenter_tier(/*reservable=*/3, /*desired=*/3, /*has_canvas_plane=*/true),
            Tier::Plane);
  EXPECT_EQ(choose_presenter_tier(5, 3, true), Tier::Plane);   // surplus overlays
  EXPECT_EQ(choose_presenter_tier(1, 1, false), Tier::Plane);  // canvas irrelevant here
}

// Short on overlays -> Composite, provided a canvas plane exists.
TEST(ChoosePresenterTier, CompositeWhenOverlaysShortButCanvasExists) {
  EXPECT_EQ(choose_presenter_tier(/*reservable=*/2, /*desired=*/3, /*has_canvas_plane=*/true),
            Tier::Composite);
  EXPECT_EQ(choose_presenter_tier(0, 4, true), Tier::Composite);
}

// Short on overlays AND no canvas plane -> nothing (caller drops to fbdev).
TEST(ChoosePresenterTier, NulloptWhenNeitherPlaneNorCanvas) {
  EXPECT_FALSE(choose_presenter_tier(/*reservable=*/1, /*desired=*/3, /*has_canvas_plane=*/false)
                   .has_value());
  EXPECT_FALSE(choose_presenter_tier(0, 1, false).has_value());
}

// desired == 0 is degenerate: never Plane (nothing to host), falls to the
// canvas check.
TEST(ChoosePresenterTier, ZeroDesiredNeverPlane) {
  EXPECT_EQ(choose_presenter_tier(0, 0, true), Tier::Composite);
  EXPECT_FALSE(choose_presenter_tier(5, 0, false).has_value());
}
