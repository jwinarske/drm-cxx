// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/unit/test_scanout_target.cpp
//
// Host unit test for the pure primary_plane_for_crtc helper, exercised against a
// synthetic PlaneRegistry (no DRM fd). discover() itself is DRM-bound and is
// covered on virtual KMS. Self-contained CHECK harness.

#include <drm-cxx/display/scanout_target.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace display = drm::display;
namespace planes = drm::planes;

static int g_fail = 0;
#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

namespace {

planes::PlaneCapabilities plane(std::uint32_t id, planes::DRMPlaneType type,
                                std::uint32_t crtc_mask) {
  planes::PlaneCapabilities cap;
  cap.id = id;
  cap.type = type;
  cap.possible_crtcs = crtc_mask;
  return cap;
}

void test_primary_plane_for_crtc() {
  std::vector<planes::PlaneCapabilities> caps;
  caps.push_back(plane(10, planes::DRMPlaneType::OVERLAY, 0b01));
  caps.push_back(plane(11, planes::DRMPlaneType::PRIMARY, 0b01));  // primary on crtc 0
  caps.push_back(plane(12, planes::DRMPlaneType::CURSOR, 0b01));
  caps.push_back(plane(13, planes::DRMPlaneType::PRIMARY, 0b10));  // primary on crtc 1
  const auto reg = planes::PlaneRegistry::from_capabilities(std::move(caps));

  const auto p0 = display::primary_plane_for_crtc(reg, 0);
  CHECK(p0.has_value());
  CHECK(p0.value_or(0) == 11);

  const auto p1 = display::primary_plane_for_crtc(reg, 1);
  CHECK(p1.has_value());
  CHECK(p1.value_or(0) == 13);

  // No plane is wired to crtc index 2.
  CHECK(!display::primary_plane_for_crtc(reg, 2).has_value());
}

}  // namespace

int main() {
  test_primary_plane_for_crtc();

  if (g_fail) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return 1;
  }
  std::puts("all scanout_target tests passed");
  return 0;
}
