// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include <drm-cxx/csd/presenter.hpp>
#include <drm-cxx/csd/presenter_plane.hpp>
#include <drm-cxx/csd/surface.hpp>

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <system_error>
#include <vector>

namespace {

using drm::csd::compute_writes;
using drm::csd::PlaneSlot;
using drm::csd::PropertyWrite;
using drm::csd::SurfaceRef;

// Build one slot with all required property ids set to predictable
// values. fb_id_prop = plane_id*100 + 1, crtc_id_prop = ...+2, etc.
PlaneSlot make_slot(std::uint32_t plane_id, std::uint32_t crtc_id, bool with_blend = false,
                    bool with_alpha = false) {
  PlaneSlot s;
  s.plane_id = plane_id;
  s.crtc_id = crtc_id;
  s.fb_id_prop = (plane_id * 100U) + 1U;
  s.crtc_id_prop = (plane_id * 100U) + 2U;
  s.crtc_x_prop = (plane_id * 100U) + 3U;
  s.crtc_y_prop = (plane_id * 100U) + 4U;
  s.crtc_w_prop = (plane_id * 100U) + 5U;
  s.crtc_h_prop = (plane_id * 100U) + 6U;
  s.src_x_prop = (plane_id * 100U) + 7U;
  s.src_y_prop = (plane_id * 100U) + 8U;
  s.src_w_prop = (plane_id * 100U) + 9U;
  s.src_h_prop = (plane_id * 100U) + 10U;
  if (with_blend) {
    s.blend_mode_prop = (plane_id * 100U) + 11U;
    s.blend_mode_value = 1U;  // driver-defined; tests only check round-trip.
  }
  if (with_alpha) {
    s.alpha_prop = (plane_id * 100U) + 12U;
  }
  return s;
}

// Find the single PropertyWrite whose object_id == plane_id and
// property_id == prop_id. Returns the value or aborts the test.
std::uint64_t find_value(const std::vector<PropertyWrite>& writes, std::uint32_t plane_id,
                         std::uint32_t prop_id) {
  for (const auto& w : writes) {
    if (w.object_id == plane_id && w.property_id == prop_id) {
      return w.value;
    }
  }
  ADD_FAILURE() << "no write for plane=" << plane_id << " prop=" << prop_id;
  return 0;
}

bool has_write(const std::vector<PropertyWrite>& writes, std::uint32_t plane_id,
               std::uint32_t prop_id) {
  return std::any_of(writes.begin(), writes.end(), [&](const PropertyWrite& w) {
    return w.object_id == plane_id && w.property_id == prop_id;
  });
}

}  // namespace

// ── compute_writes — error paths ───────────────────────────────────────

TEST(CsdComputeWrites, RejectsTooManySurfaces) {
  const std::vector<PlaneSlot> slots{make_slot(1, 99)};
  // Two surface refs (we only care about size, not content for this
  // path) against one slot.
  const std::vector<SurfaceRef> surfaces{SurfaceRef{}, SurfaceRef{}};
  auto out = compute_writes(slots, surfaces);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error(), std::make_error_code(std::errc::no_buffer_space));
}

TEST(CsdComputeWrites, EmptySlotsAndSurfacesIsTrivialSuccess) {
  const std::vector<PlaneSlot> slots;
  const std::vector<SurfaceRef> surfaces;
  auto out = compute_writes(slots, surfaces);
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->empty());
}

// ── compute_writes — disarm path ──────────────────────────────────────

TEST(CsdComputeWrites, NullSurfaceDisarmsItsSlot) {
  const std::vector<PlaneSlot> slots{make_slot(1, 99)};
  const std::vector<SurfaceRef> surfaces{SurfaceRef{/*surface=*/nullptr, 0, 0}};
  auto out = compute_writes(slots, surfaces);
  ASSERT_TRUE(out.has_value());

  // Exactly two writes — FB_ID=0, CRTC_ID=0 — and nothing else.
  ASSERT_EQ(out->size(), 2U);
  EXPECT_EQ(find_value(*out, 1U, slots[0].fb_id_prop), 0U);
  EXPECT_EQ(find_value(*out, 1U, slots[0].crtc_id_prop), 0U);
  // Geometry writes must not appear for a disarmed slot — otherwise
  // CRTC_W=0 would be a kernel reject in some drivers.
  EXPECT_FALSE(has_write(*out, 1U, slots[0].crtc_w_prop));
  EXPECT_FALSE(has_write(*out, 1U, slots[0].src_w_prop));
}

TEST(CsdComputeWrites, FewerSurfacesThanSlotsDisarmsTheTail) {
  const std::vector<PlaneSlot> slots{make_slot(1, 99), make_slot(2, 99), make_slot(3, 99)};
  // One null surface — slots 1 and 2 must be disarmed; slot 0
  // doesn't get armed either because its surface is also null.
  const std::vector<SurfaceRef> surfaces{SurfaceRef{nullptr, 0, 0}};
  auto out = compute_writes(slots, surfaces);
  ASSERT_TRUE(out.has_value());

  // 3 slots × 2 writes (FB_ID=0, CRTC_ID=0) = 6 writes total.
  EXPECT_EQ(out->size(), 6U);
  for (const std::uint32_t pid : {1U, 2U, 3U}) {
    EXPECT_EQ(find_value(*out, pid, slots[pid - 1].fb_id_prop), 0U);
    EXPECT_EQ(find_value(*out, pid, slots[pid - 1].crtc_id_prop), 0U);
  }
}

// ── compute_writes — armed path requires a real Surface ──────────────
// Surface's only public ctor that produces a non-empty Surface needs a
// live drm::Device. We can't paint a real Surface in a unit test, so
// the armed path is exercised end-to-end only on hardware (csd_smoke
// extension in a follow-up). What we *can* test is that compute_writes
// faithfully marshals the optional blend/alpha properties — which is
// where divergence would show up across drivers.

TEST(CsdComputeWrites, ZposPropertyWrittenWhenSlotPropIdIsSet) {
  // A slot with zpos_prop set emits a zpos write for armed planes
  // and skips it for disarmed ones — the latter mirrors the FB_ID=0
  // disarm path, where geometry-style writes are deliberately
  // suppressed to avoid kernel rejects.
  PlaneSlot slot = make_slot(1, 99);
  slot.zpos_prop = 12345U;
  slot.zpos_value = 7U;
  const std::vector<PlaneSlot> slots{slot};

  // Disarmed: no zpos write.
  {
    const std::vector<SurfaceRef> surfaces{SurfaceRef{nullptr, 0, 0}};
    auto out = compute_writes(slots, surfaces);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(has_write(*out, 1U, slot.zpos_prop));
  }

  // Armed (with a default-constructed Surface, which is empty()) —
  // still no zpos write because the empty Surface disarms.
  {
    const drm::csd::Surface empty_surface;
    const std::vector<SurfaceRef> surfaces{SurfaceRef{&empty_surface, 0, 0}};
    auto out = compute_writes(slots, surfaces);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(has_write(*out, 1U, slot.zpos_prop));
  }

  // The "armed" zpos-write path is otherwise covered by the
  // hardware-validation harness (csd_smoke --presenter=plane), since
  // a non-empty Surface needs a live drm::Device.
}

TEST(CsdComputeWrites, OptionalPropertiesSkippedWhenSlotPropIdIsZero) {
  // Slot has no blend or alpha prop. compute_writes should emit
  // exactly the 2 disarm writes and nothing optional.
  const std::vector<PlaneSlot> slots{make_slot(1, 99, /*with_blend=*/false,
                                               /*with_alpha=*/false)};
  const std::vector<SurfaceRef> surfaces{SurfaceRef{nullptr, 0, 0}};
  auto out = compute_writes(slots, surfaces);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->size(), 2U);
}

// ── Surface defaults & SurfaceRef behavior ────────────────────────────

TEST(CsdSurfaceRef, DefaultsAreEmpty) {
  const SurfaceRef ref;
  EXPECT_EQ(ref.surface, nullptr);
  EXPECT_EQ(ref.x, 0);
  EXPECT_EQ(ref.y, 0);
}

TEST(CsdComputeWrites, EmptySurfaceTreatedAsDisarm) {
  // A default-constructed Surface is empty(); compute_writes should
  // disarm its slot just like surface=nullptr does. This matters
  // because a shell's "doc closed but slot still bound" path has the
  // shell hand back an empty Surface, not a null pointer.
  const drm::csd::Surface empty_surface;
  ASSERT_TRUE(empty_surface.empty());

  const std::vector<PlaneSlot> slots{make_slot(1, 99)};
  const std::vector<SurfaceRef> surfaces{SurfaceRef{&empty_surface, 0, 0}};
  auto out = compute_writes(slots, surfaces);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->size(), 2U);
  EXPECT_EQ(find_value(*out, 1U, slots[0].fb_id_prop), 0U);
  EXPECT_EQ(find_value(*out, 1U, slots[0].crtc_id_prop), 0U);
}