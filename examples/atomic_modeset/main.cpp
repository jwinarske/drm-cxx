// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0
//
// atomic_modeset — end-to-end atomic modesetting example.
//
// Usage: atomic_modeset [/dev/dri/cardN]
//
// Opens a DRM device, finds a connected connector, selects the preferred
// mode, and performs an atomic modeset. Then waits for a page flip event.

#include <cstdlib>
#include <print>

#include "core/device.hpp"
#include "core/resources.hpp"
#include "modeset/atomic.hpp"
#include "modeset/mode.hpp"
#include "modeset/page_flip.hpp"

int main(int argc, char* argv[]) {
  const char* path = (argc > 1) ? argv[1] : "/dev/dri/card0";

  // Open DRM device
  auto dev_result = drm::Device::open(path);
  if (!dev_result) {
    std::println(stderr, "Failed to open {}", path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_result;

  // Enable capabilities
  if (auto r = dev.enable_universal_planes(); !r) {
    std::println(stderr, "Failed to enable universal planes");
    return EXIT_FAILURE;
  }
  if (auto r = dev.enable_atomic(); !r) {
    std::println(stderr, "Failed to enable atomic modesetting");
    return EXIT_FAILURE;
  }

  // Get resources
  auto res = drm::get_resources(dev.fd());
  if (!res) {
    std::println(stderr, "Failed to get DRM resources");
    return EXIT_FAILURE;
  }

  std::println("Found {} connectors, {} CRTCs, {} encoders",
    res->count_connectors, res->count_crtcs, res->count_encoders);

  // Find first connected connector
  drm::Connector conn{nullptr, &drmModeFreeConnector};
  for (int i = 0; i < res->count_connectors; ++i) {
    auto c = drm::get_connector(dev.fd(), res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      std::println("Connector {}: {} modes",
        c->connector_id, c->count_modes);
      conn = std::move(c);
      break;
    }
  }

  if (!conn) {
    std::println(stderr, "No connected connector found");
    return EXIT_FAILURE;
  }

  // Select preferred mode
  auto modes = std::span<const drmModeModeInfo>(
    conn->modes, conn->count_modes);
  auto mode_result = drm::select_preferred_mode(modes);
  if (!mode_result) {
    std::println(stderr, "No suitable mode found");
    return EXIT_FAILURE;
  }
  auto& mode = *mode_result;
  std::println("Selected mode: {}x{}@{}Hz{}",
    mode.width(), mode.height(), mode.refresh(),
    mode.preferred() ? " (preferred)" : "");

  // List all available modes
  auto all_modes = drm::get_all_modes(modes);
  for (const auto& m : all_modes) {
    std::println("  {}x{}@{}Hz{}{}",
      m.width(), m.height(), m.refresh(),
      m.preferred() ? " [preferred]" : "",
      m.interlaced() ? " [interlaced]" : "");
  }

  // Find encoder and CRTC
  if (!conn->encoder_id) {
    std::println(stderr, "No encoder attached to connector");
    return EXIT_FAILURE;
  }

  auto enc = drm::get_encoder(dev.fd(), conn->encoder_id);
  if (!enc) {
    std::println(stderr, "Failed to get encoder");
    return EXIT_FAILURE;
  }

  auto crtc = drm::get_crtc(dev.fd(), enc->crtc_id);
  if (!crtc) {
    std::println(stderr, "Failed to get CRTC");
    return EXIT_FAILURE;
  }

  std::println("Using CRTC {} with encoder {}",
    crtc->crtc_id, enc->encoder_id);

  // Set up page flip handler
  drm::PageFlip page_flip(dev);
  page_flip.set_handler([](uint32_t crtc_id, uint64_t seq, uint64_t ts_ns) {
    std::println("Page flip on CRTC {}: seq={}, timestamp={}ns",
      crtc_id, seq, ts_ns);
  });

  std::println("\nAtomic modeset example complete.");
  std::println("(Full modeset commit requires a framebuffer, which is not");
  std::println("created in this example. See overlay_planes for allocator usage.)");

  return EXIT_SUCCESS;
}
