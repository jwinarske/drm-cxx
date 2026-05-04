// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include <drm-cxx/csd/overlay_reservation.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm_fourcc.h>

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using drm::csd::OverlayReservation;
using drm::planes::DRMPlaneType;
using drm::planes::PlaneCapabilities;
using drm::planes::PlaneRegistry;

// Build a single-format OVERLAY plane with the requested CRTC mask and
// zpos. Tests want a quick fixture builder, not a full driver model.
PlaneCapabilities make_overlay(std::uint32_t id, std::uint32_t possible_crtcs, std::uint64_t zpos,
                               std::uint32_t format = DRM_FORMAT_ARGB8888) {
  PlaneCapabilities caps;
  caps.id = id;
  caps.possible_crtcs = possible_crtcs;
  caps.type = DRMPlaneType::OVERLAY;
  caps.formats = {format};
  caps.zpos_min = zpos;
  caps.zpos_max = zpos;
  return caps;
}

PlaneCapabilities make_primary(std::uint32_t id, std::uint32_t possible_crtcs, std::uint64_t zpos) {
  PlaneCapabilities caps;
  caps.id = id;
  caps.possible_crtcs = possible_crtcs;
  caps.type = DRMPlaneType::PRIMARY;
  caps.formats = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
  caps.zpos_min = zpos;
  caps.zpos_max = zpos;
  return caps;
}

PlaneCapabilities make_cursor(std::uint32_t id, std::uint32_t possible_crtcs, std::uint64_t zpos) {
  PlaneCapabilities caps;
  caps.id = id;
  caps.possible_crtcs = possible_crtcs;
  caps.type = DRMPlaneType::CURSOR;
  caps.formats = {DRM_FORMAT_ARGB8888};
  caps.zpos_min = zpos;
  caps.zpos_max = zpos;
  return caps;
}

// amdgpu DCN shape: 2 CRTCs, partitioned planes (each plane belongs to
// exactly one CRTC). Three overlays per CRTC, one primary, one cursor.
PlaneRegistry partitioned_two_crtc_registry() {
  std::vector<PlaneCapabilities> caps;
  // CRTC 0: primary @ zpos 0, overlays @ 1..3, cursor @ 4.
  caps.push_back(make_primary(10, 0b01, 0));
  caps.push_back(make_overlay(11, 0b01, 1));
  caps.push_back(make_overlay(12, 0b01, 2));
  caps.push_back(make_overlay(13, 0b01, 3));
  caps.push_back(make_cursor(14, 0b01, 4));
  // CRTC 1: same shape.
  caps.push_back(make_primary(20, 0b10, 0));
  caps.push_back(make_overlay(21, 0b10, 1));
  caps.push_back(make_overlay(22, 0b10, 2));
  caps.push_back(make_overlay(23, 0b10, 3));
  caps.push_back(make_cursor(24, 0b10, 4));
  return PlaneRegistry::from_capabilities(std::move(caps));
}

// Mali Komeda shape: 2 CRTCs, all overlays routable to either. Six
// shared overlays plus per-CRTC primary + cursor.
PlaneRegistry shared_plane_registry() {
  std::vector<PlaneCapabilities> caps;
  caps.push_back(make_primary(10, 0b01, 0));
  caps.push_back(make_cursor(11, 0b01, 9));
  caps.push_back(make_primary(20, 0b10, 0));
  caps.push_back(make_cursor(21, 0b10, 9));
  // Six shared overlays, possible_crtcs = both, ascending zpos so the
  // zpos sort order is well-defined.
  for (std::uint32_t i = 0; i < 6; ++i) {
    caps.push_back(make_overlay(100 + i, 0b11, /*zpos=*/1 + i));
  }
  return PlaneRegistry::from_capabilities(std::move(caps));
}

}  // namespace

// ── Construction ───────────────────────────────────────────────────────

TEST(OverlayReservationTest, CreateReturnsBoundReservation) {
  const PlaneRegistry registry = partitioned_two_crtc_registry();
  auto res = OverlayReservation::create(registry);
  ASSERT_TRUE(res.has_value());
  // Newly constructed: nothing reserved anywhere.
  EXPECT_TRUE(res->all_reserved().empty());
  EXPECT_TRUE(res->reserved_for(0).empty());
}

TEST(OverlayReservationTest, DefaultConstructedReserveFails) {
  OverlayReservation res;
  auto out = res.reserve(0, DRM_FORMAT_ARGB8888, 1);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error(), std::make_error_code(std::errc::invalid_argument));
}

// ── Partitioned (amdgpu) — independent per-CRTC pools ──────────────────

TEST(OverlayReservationTest, PartitionedReservesPerCrtcWithoutCoupling) {
  const PlaneRegistry registry = partitioned_two_crtc_registry();
  auto res = *OverlayReservation::create(registry);

  auto a = res.reserve(/*crtc_index=*/0, DRM_FORMAT_ARGB8888, /*count=*/3, /*min_zpos=*/1);
  ASSERT_TRUE(a.has_value());
  ASSERT_EQ(a->size(), 3U);
  EXPECT_EQ((*a)[0], 11U);  // zpos 1
  EXPECT_EQ((*a)[1], 12U);  // zpos 2
  EXPECT_EQ((*a)[2], 13U);  // zpos 3

  // CRTC 1's pool is disjoint — its full 3-overlay budget remains.
  auto b = res.reserve(/*crtc_index=*/1, DRM_FORMAT_ARGB8888, /*count=*/3, /*min_zpos=*/1);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(b->size(), 3U);
  EXPECT_EQ((*b)[0], 21U);
  EXPECT_EQ((*b)[1], 22U);
  EXPECT_EQ((*b)[2], 23U);

  // Union of all reservations is the six overlays we asked for, no more.
  auto all = res.all_reserved();
  std::sort(all.begin(), all.end());
  EXPECT_EQ(all, (std::vector<std::uint32_t>{11, 12, 13, 21, 22, 23}));
}

TEST(OverlayReservationTest, PartitionedShortfallReturnsTryAgain) {
  const PlaneRegistry registry = partitioned_two_crtc_registry();
  auto res = *OverlayReservation::create(registry);

  // CRTC 0 only has 3 overlays; ask for 4.
  auto out = res.reserve(0, DRM_FORMAT_ARGB8888, 4, /*min_zpos=*/1);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error(), std::make_error_code(std::errc::resource_unavailable_try_again));

  // Failure must not leak partial state — nothing reserved.
  EXPECT_TRUE(res.reserved_for(0).empty());
  EXPECT_TRUE(res.all_reserved().empty());
}

// ── Shared planes (Komeda) — first-come-first-served exclusion ─────────

TEST(OverlayReservationTest, SharedPlanesFirstReservationConsumesPool) {
  const PlaneRegistry registry = shared_plane_registry();
  auto res = *OverlayReservation::create(registry);

  auto a = res.reserve(0, DRM_FORMAT_ARGB8888, 4, /*min_zpos=*/1);
  ASSERT_TRUE(a.has_value());
  ASSERT_EQ(a->size(), 4U);
  // Returned in ascending zpos order: first four shared planes.
  EXPECT_EQ((*a)[0], 100U);
  EXPECT_EQ((*a)[1], 101U);
  EXPECT_EQ((*a)[2], 102U);
  EXPECT_EQ((*a)[3], 103U);

  // CRTC 1 sees only the remaining two — asking for four reports
  // shortfall.
  auto b_short = res.reserve(1, DRM_FORMAT_ARGB8888, 4, /*min_zpos=*/1);
  ASSERT_FALSE(b_short.has_value());
  EXPECT_EQ(b_short.error(), std::make_error_code(std::errc::resource_unavailable_try_again));

  // Asking for the residual two succeeds.
  auto b = res.reserve(1, DRM_FORMAT_ARGB8888, 2, /*min_zpos=*/1);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ((*b)[0], 104U);
  EXPECT_EQ((*b)[1], 105U);
}

// ── Hotplug release ────────────────────────────────────────────────────

TEST(OverlayReservationTest, ReleaseFreesPlanesForOtherCrtc) {
  const PlaneRegistry registry = shared_plane_registry();
  auto res = *OverlayReservation::create(registry);

  ASSERT_TRUE(res.reserve(0, DRM_FORMAT_ARGB8888, 4, /*min_zpos=*/1).has_value());
  ASSERT_TRUE(res.reserve(1, DRM_FORMAT_ARGB8888, 2, /*min_zpos=*/1).has_value());

  // CRTC 1 unplugs.
  res.release(1);
  EXPECT_TRUE(res.reserved_for(1).empty());

  // The two planes CRTC 1 had go back to the free pool. A hypothetical
  // CRTC 1 reattach now sees them again, in the same zpos order.
  auto b = res.reserve(1, DRM_FORMAT_ARGB8888, 2, /*min_zpos=*/1);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ((*b)[0], 104U);
  EXPECT_EQ((*b)[1], 105U);
}

TEST(OverlayReservationTest, ReleaseIsIdempotent) {
  const PlaneRegistry registry = partitioned_two_crtc_registry();
  auto res = *OverlayReservation::create(registry);

  // Releasing a CRTC that was never reserved is a no-op, not an error.
  res.release(0);
  res.release(0);
  res.release(99);
  EXPECT_TRUE(res.all_reserved().empty());
}

TEST(OverlayReservationTest, ReReserveDropsPriorEntry) {
  const PlaneRegistry registry = partitioned_two_crtc_registry();
  auto res = *OverlayReservation::create(registry);

  ASSERT_TRUE(res.reserve(0, DRM_FORMAT_ARGB8888, 2, /*min_zpos=*/1).has_value());
  EXPECT_EQ(res.reserved_for(0).size(), 2U);

  // Reserving again on the same CRTC drops the previous entry rather
  // than stacking — leaking would be silent and surface only as an
  // exhausted free pool many calls later.
  ASSERT_TRUE(res.reserve(0, DRM_FORMAT_ARGB8888, 3, /*min_zpos=*/1).has_value());
  EXPECT_EQ(res.reserved_for(0).size(), 3U);
  EXPECT_EQ(res.all_reserved().size(), 3U);
}

// ── Filtering rules ────────────────────────────────────────────────────

TEST(OverlayReservationTest, ZposFloorExcludesPrimary) {
  const PlaneRegistry registry = partitioned_two_crtc_registry();
  auto res = *OverlayReservation::create(registry);

  // min_zpos=1 excludes the primary (zpos 0). Even at min_zpos=0 the
  // primary should still be excluded — it isn't OVERLAY-typed.
  auto a = res.reserve(0, DRM_FORMAT_ARGB8888, 3, /*min_zpos=*/0);
  ASSERT_TRUE(a.has_value());
  for (const std::uint32_t id : *a) {
    EXPECT_NE(id, 10U);  // primary id
    EXPECT_NE(id, 14U);  // cursor id
  }
}

TEST(OverlayReservationTest, FormatMismatchReducesPool) {
  // Build a CRTC whose three overlays advertise different formats: one
  // ARGB8888, two RGB565. The reservation only sees the one ARGB plane.
  std::vector<PlaneCapabilities> caps;
  caps.push_back(make_primary(10, 0b1, 0));
  caps.push_back(make_overlay(11, 0b1, 1, DRM_FORMAT_ARGB8888));
  caps.push_back(make_overlay(12, 0b1, 2, DRM_FORMAT_RGB565));
  caps.push_back(make_overlay(13, 0b1, 3, DRM_FORMAT_RGB565));
  const PlaneRegistry registry = PlaneRegistry::from_capabilities(std::move(caps));
  auto res = *OverlayReservation::create(registry);

  auto out = res.reserve(0, DRM_FORMAT_ARGB8888, 2, /*min_zpos=*/1);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error(), std::make_error_code(std::errc::resource_unavailable_try_again));

  auto one = res.reserve(0, DRM_FORMAT_ARGB8888, 1, /*min_zpos=*/1);
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ((*one)[0], 11U);
}

TEST(OverlayReservationTest, PlanesWithoutZposAreSkipped) {
  // A plane with no zpos property at all can't be ordered against the
  // primary. The reservation skips it rather than emitting it as an
  // unsorted slot — callers asking for "above primary" would otherwise
  // get a plane whose actual scanout order is driver-defined.
  std::vector<PlaneCapabilities> caps;
  caps.push_back(make_primary(10, 0b1, 0));
  PlaneCapabilities no_zpos;
  no_zpos.id = 11;
  no_zpos.possible_crtcs = 0b1;
  no_zpos.type = DRMPlaneType::OVERLAY;
  no_zpos.formats = {DRM_FORMAT_ARGB8888};
  // zpos_min / zpos_max left as nullopt
  caps.push_back(std::move(no_zpos));
  caps.push_back(make_overlay(12, 0b1, 2));
  const PlaneRegistry registry = PlaneRegistry::from_capabilities(std::move(caps));
  auto res = *OverlayReservation::create(registry);

  // Only one zpos-bearing overlay is visible.
  auto out = res.reserve(0, DRM_FORMAT_ARGB8888, 1, /*min_zpos=*/1);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ((*out)[0], 12U);

  // Asking for two reports shortfall, not a silent inclusion of the
  // zpos-less plane.
  auto two = res.reserve(0, DRM_FORMAT_ARGB8888, 2, /*min_zpos=*/1);
  ASSERT_FALSE(two.has_value());
}

// ── Diagnostic accessors ───────────────────────────────────────────────

TEST(OverlayReservationTest, ReservedForReturnsStableSpanInZposOrder) {
  const PlaneRegistry registry = partitioned_two_crtc_registry();
  auto res = *OverlayReservation::create(registry);

  ASSERT_TRUE(res.reserve(0, DRM_FORMAT_ARGB8888, 2, /*min_zpos=*/1).has_value());
  auto span = res.reserved_for(0);
  ASSERT_EQ(span.size(), 2U);
  EXPECT_EQ(span[0], 11U);
  EXPECT_EQ(span[1], 12U);

  // Unrelated CRTC has nothing reserved — empty span, not an error.
  EXPECT_TRUE(res.reserved_for(99).empty());
}

TEST(OverlayReservationTest, ZeroCountReservationIsTrivialSuccess) {
  const PlaneRegistry registry = partitioned_two_crtc_registry();
  auto res = *OverlayReservation::create(registry);

  // count=0 is the "this presenter doesn't want any planes" path —
  // succeeds without consuming the pool.
  auto out = res.reserve(0, DRM_FORMAT_ARGB8888, 0, /*min_zpos=*/1);
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->empty());
  EXPECT_TRUE(res.all_reserved().empty());
}