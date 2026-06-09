// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/unit/test_driver_profile.cpp
//
// Host unit test for the pure decode_prime_caps helper and DriverProfile
// defaults. probe() is DRM-bound and is covered live on virtual KMS.

#include <drm-cxx/display/driver_profile.hpp>

#include <drm.h>
#include <drm_mode.h>

#include <cstdio>

namespace display = drm::display;

static int g_fail = 0;
#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

namespace {

void test_decode_prime_caps() {
  CHECK(!display::decode_prime_caps(0).can_import);
  CHECK(!display::decode_prime_caps(0).can_export);

  const auto imp = display::decode_prime_caps(DRM_PRIME_CAP_IMPORT);
  CHECK(imp.can_import);
  CHECK(!imp.can_export);

  const auto exp = display::decode_prime_caps(DRM_PRIME_CAP_EXPORT);
  CHECK(!exp.can_import);
  CHECK(exp.can_export);

  const auto both = display::decode_prime_caps(DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT);
  CHECK(both.can_import);
  CHECK(both.can_export);
}

void test_defaults() {
  const display::DriverProfile p;
  CHECK(p.name.empty());
  CHECK(!p.addfb2_modifiers);
  CHECK(!p.async_page_flip);
  CHECK(p.cursor_width == 64);  // 64 fallback when undefined
  CHECK(p.cursor_height == 64);
  // Frame-economy caps default conservative (probe sets them when present).
  CHECK(!p.fb_damage_clips);
  CHECK(!p.vrr_capable);
  CHECK(p.psr == display::PanelSelfRefresh::Unknown);
}

void test_connector_type_self_refreshes() {
  // Embedded panels can self-refresh.
  CHECK(display::connector_type_self_refreshes(DRM_MODE_CONNECTOR_eDP));
  CHECK(display::connector_type_self_refreshes(DRM_MODE_CONNECTOR_DSI));
  // External connectors cannot.
  CHECK(!display::connector_type_self_refreshes(DRM_MODE_CONNECTOR_HDMIA));
  CHECK(!display::connector_type_self_refreshes(DRM_MODE_CONNECTOR_DisplayPort));
  CHECK(!display::connector_type_self_refreshes(DRM_MODE_CONNECTOR_VGA));
  CHECK(!display::connector_type_self_refreshes(DRM_MODE_CONNECTOR_Unknown));
}

}  // namespace

int main() {
  test_decode_prime_caps();
  test_defaults();
  test_connector_type_self_refreshes();

  if (g_fail) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return 1;
  }
  std::puts("all driver_profile tests passed");
  return 0;
}
