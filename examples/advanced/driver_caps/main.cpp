// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// driver_caps — dump the DriverProfile for a DRM node, including the
// frame-economy capabilities (FB_DAMAGE_CLIPS advertisement, VRR drivability,
// and the best-effort PSR telemetry) and a plane-type census. A read-only
// diagnostic; needs no DRM master and issues no modeset (enabling the
// universal-planes client cap and reading plane resources are both
// master-free).
//
//   ./driver_caps [/dev/dri/cardN]

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/display/driver_profile.hpp>
#include <drm-cxx/display/mode_list.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <cstdlib>
#include <string>

namespace {

const char* psr_str(drm::display::PanelSelfRefresh p) {
  switch (p) {
    case drm::display::PanelSelfRefresh::Possible:
      return "possible (connected eDP/DSI)";
    case drm::display::PanelSelfRefresh::None:
      return "none (external connector)";
    case drm::display::PanelSelfRefresh::Unknown:
      break;
  }
  return "unknown";
}

}  // namespace

int main(int argc, char** argv) try {
  const std::string path = (argc > 1) ? argv[1] : "/dev/dri/card0";
  auto dev = drm::Device::open(path);
  if (!dev) {
    drm::println(stderr, "driver_caps: open {}: {}", path, dev.error().message());
    return EXIT_FAILURE;
  }
  auto prof = drm::display::DriverProfile::probe(*dev);
  if (!prof) {
    drm::println(stderr, "driver_caps: probe: {}", prof.error().message());
    return EXIT_FAILURE;
  }

  drm::println("driver_caps: {} ({})", prof->name, path);
  drm::println("  addfb2_modifiers : {}", prof->addfb2_modifiers);
  drm::println("  async_page_flip  : {}", prof->async_page_flip);
  drm::println("  prime import/exp : {}/{}", prof->prime_import, prof->prime_export);
  drm::println("  cursor max       : {}x{}", prof->cursor_width, prof->cursor_height);
  drm::println("  -- frame economy --");
  drm::println("  fb_damage_clips  : {}", prof->fb_damage_clips);
  drm::println("  vrr_capable      : {}", prof->vrr_capable);
  drm::println("  psr              : {}", psr_str(prof->psr));

  // Plane-type census + cursor-cap sanity check. The registry only sees the
  // PRIMARY/CURSOR planes once DRM_CLIENT_CAP_UNIVERSAL_PLANES is set — without
  // it, drmModeGetPlaneResources hides all but legacy overlays and the cursor
  // check below would false-positive on every driver. Setting the cap is
  // master-free, so the read-only contract holds.
  if (auto r = dev->enable_universal_planes(); !r) {
    drm::println(stderr, "  planes: enable_universal_planes: {}", r.error().message());
  } else if (auto reg = drm::planes::PlaneRegistry::enumerate(*dev)) {
    int n_primary = 0;
    int n_overlay = 0;
    int n_cursor = 0;
    for (const auto& pc : reg->all()) {
      switch (pc.type) {
        case drm::planes::DRMPlaneType::PRIMARY:
          ++n_primary;
          break;
        case drm::planes::DRMPlaneType::OVERLAY:
          ++n_overlay;
          break;
        case drm::planes::DRMPlaneType::CURSOR:
          ++n_cursor;
          break;
      }
    }
    drm::println("  -- planes --");
    drm::println("  census           : PRIMARY={} OVERLAY={} CURSOR={}", n_primary, n_overlay,
                 n_cursor);
    // DRM_CAP_CURSOR_* defaults to 64x64 even on controllers with no cursor
    // plane (e.g. i.MX LCDIF, tilcdc). HW-cursor use must be gated on an actual
    // CURSOR plane in the registry, not on the cap, or the cursor path arms a
    // plane that does not exist.
    if (n_cursor == 0 && (prof->cursor_width != 0 || prof->cursor_height != 0)) {
      drm::println(
          "  [WARN] DRM_CAP_CURSOR_* reports {}x{} but no CURSOR plane exists — "
          "gate HW cursor on the registry, not the cap.",
          prof->cursor_width, prof->cursor_height);
    }
  } else {
    drm::println(stderr, "  planes: enumerate: {}", reg.error().message());
  }

  // Connectors + advertised modes (display/mode_list).
  if (auto conns = drm::display::query_connector_modes(*dev)) {
    drm::println("  -- connectors --");
    for (const auto& c : *conns) {
      drm::println("  {:<10} id={:<3} {:<12} modes={}", c.name(), c.connector_id,
                   c.connected ? "connected" : "disconnected", c.modes.size());
      if (!c.modes.empty()) {
        const auto& m = c.modes.front();
        drm::println("      first: {}x{}@{}{}", m.width(), m.height(), m.refresh(),
                     m.preferred() ? " (preferred)" : "");
      }
    }
  } else {
    drm::println(stderr, "  connectors: {}", conns.error().message());
  }
  return EXIT_SUCCESS;
} catch (...) {
  return EXIT_FAILURE;
}
