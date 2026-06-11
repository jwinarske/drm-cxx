// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// driver_caps — dump the DriverProfile for a DRM node, including the
// frame-economy capabilities (FB_DAMAGE_CLIPS advertisement, VRR drivability,
// and the best-effort PSR telemetry). A read-only diagnostic; needs no DRM
// master and issues no modeset.
//
//   ./driver_caps [/dev/dri/cardN]

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/display/driver_profile.hpp>
#include <drm-cxx/display/mode_list.hpp>

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

int main(int argc, char** argv) {
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
}
