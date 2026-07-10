// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include <drm-cxx/csd/presenter_composite.hpp>
#include <drm-cxx/csd/presenter_plane.hpp>

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace {

using drm::csd::compute_canvas_writes;
using drm::csd::PlaneSlot;
using drm::csd::PropertyWrite;

// One canvas plane slot with predictable property ids:
// fb_id_prop = plane_id*100 + 1, crtc_id_prop = +2, ... src_h_prop = +10.
PlaneSlot make_canvas_slot(std::uint32_t plane_id, std::uint32_t crtc_id) {
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
  return s;
}

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

}  // namespace

// ── compute_canvas_writes — always full-screen, always armed ───────────

TEST(CsdComputeCanvasWrites, ArmsFullScreenGeometry) {
  const PlaneSlot slot = make_canvas_slot(7, 42);
  const auto writes = compute_canvas_writes(slot, /*fb_id=*/1234, /*canvas_w=*/1920,
                                            /*canvas_h=*/1080);

  // Exactly the ten geometry writes — no optional props, no disarm.
  ASSERT_EQ(writes.size(), 10U);

  EXPECT_EQ(find_value(writes, 7U, slot.fb_id_prop), 1234U);
  EXPECT_EQ(find_value(writes, 7U, slot.crtc_id_prop), 42U);

  // Destination covers the whole CRTC at the origin.
  EXPECT_EQ(find_value(writes, 7U, slot.crtc_x_prop), 0U);
  EXPECT_EQ(find_value(writes, 7U, slot.crtc_y_prop), 0U);
  EXPECT_EQ(find_value(writes, 7U, slot.crtc_w_prop), 1920U);
  EXPECT_EQ(find_value(writes, 7U, slot.crtc_h_prop), 1080U);

  // Source covers the whole canvas; SRC_W/H are 16.16 fixed-point.
  EXPECT_EQ(find_value(writes, 7U, slot.src_x_prop), 0U);
  EXPECT_EQ(find_value(writes, 7U, slot.src_y_prop), 0U);
  EXPECT_EQ(find_value(writes, 7U, slot.src_w_prop), 1920ULL << 16U);
  EXPECT_EQ(find_value(writes, 7U, slot.src_h_prop), 1080ULL << 16U);
}

TEST(CsdComputeCanvasWrites, EveryWriteTargetsTheCanvasPlane) {
  const PlaneSlot slot = make_canvas_slot(3, 9);
  const auto writes = compute_canvas_writes(slot, 55, 640, 480);
  for (const auto& w : writes) {
    EXPECT_EQ(w.object_id, 3U) << "canvas writes must all target the canvas plane";
  }
}

// A zero fb_id is still emitted (the presenter never disarms the canvas;
// a 0 here would only occur pre-first-flush and the caller controls that).
TEST(CsdComputeCanvasWrites, EmitsFbIdVerbatim) {
  const PlaneSlot slot = make_canvas_slot(1, 1);
  EXPECT_EQ(find_value(compute_canvas_writes(slot, 0, 800, 600), 1U, slot.fb_id_prop), 0U);
  EXPECT_EQ(find_value(compute_canvas_writes(slot, 99, 800, 600), 1U, slot.fb_id_prop), 99U);
}
