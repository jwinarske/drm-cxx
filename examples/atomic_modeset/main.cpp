// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// atomic_modeset — end-to-end atomic modesetting example.
//
// Usage: atomic_modeset [/dev/dri/cardN]
//
// Opens a DRM device, finds a connected connector, selects the preferred
// mode, and performs an atomic modeset. Then waits for a page flip event.

#include "../select_device.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "drm-cxx/detail/format.hpp"
#include "modeset/atomic.hpp"
#include "modeset/mode.hpp"
#include "modeset/page_flip.hpp"

#include <drm-cxx/detail/span.hpp>

#include <xf86drmMode.h>

#include <cstdint>
#include <cstdlib>
#include <utility>

int main(const int argc, char* argv[]) {
  const auto path = drm::examples::select_device(argc, argv);
  if (!path) {
    return EXIT_FAILURE;
  }

  // Open DRM device
  auto dev_result = drm::Device::open(*path);
  if (!dev_result) {
    drm::println(stderr, "Failed to open {}", *path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_result;

  // Enable capabilities
  if (const auto r = dev.enable_universal_planes(); !r) {
    drm::println(stderr, "Failed to enable universal planes");
    return EXIT_FAILURE;
  }
  if (const auto r = dev.enable_atomic(); !r) {
    drm::println(stderr, "Failed to enable atomic modesetting");
    return EXIT_FAILURE;
  }

  // Get resources
  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    drm::println(stderr, "Failed to get DRM resources");
    return EXIT_FAILURE;
  }

  drm::println("Found {} connectors, {} CRTCs, {} encoders", res->count_connectors,
               res->count_crtcs, res->count_encoders);

  // Find the first connected connector
  drm::Connector conn{nullptr, &drmModeFreeConnector};
  for (int i = 0; i < res->count_connectors; ++i) {
    if (auto c = drm::get_connector(dev.fd(), res->connectors[i]);
        c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      drm::println("Connector {}: {} modes", c->connector_id, c->count_modes);
      conn = std::move(c);
      break;
    }
  }

  if (!conn) {
    drm::println(stderr, "No connected connector found");
    return EXIT_FAILURE;
  }

  // Select preferred mode
  const auto modes = drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes);
  const auto mode_result = drm::select_preferred_mode(modes);
  if (!mode_result) {
    drm::println(stderr, "No suitable mode found");
    return EXIT_FAILURE;
  }
  const auto& mode = *mode_result;
  drm::println("Selected mode: {}x{}@{}Hz{}", mode.width(), mode.height(), mode.refresh(),
               mode.preferred() ? " (preferred)" : "");

  // List all available modes
  const auto all_modes = drm::get_all_modes(modes);
  for (const auto& m : all_modes) {
    drm::println("  {}x{}@{}Hz{}{}", m.width(), m.height(), m.refresh(),
                 m.preferred() ? " [preferred]" : "", m.interlaced() ? " [interlaced]" : "");
  }

  // Find encoder and CRTC
  if (conn->encoder_id == 0U) {
    drm::println(stderr, "No encoder attached to connector");
    return EXIT_FAILURE;
  }

  const auto enc = drm::get_encoder(dev.fd(), conn->encoder_id);
  if (!enc) {
    drm::println(stderr, "Failed to get encoder");
    return EXIT_FAILURE;
  }

  auto crtc = drm::get_crtc(dev.fd(), enc->crtc_id);
  if (!crtc) {
    drm::println(stderr, "Failed to get CRTC");
    return EXIT_FAILURE;
  }

  drm::println("Using CRTC {} with encoder {}", crtc->crtc_id, enc->encoder_id);

  // Set up page flip handler
  drm::PageFlip page_flip(dev);
  page_flip.set_handler([](uint32_t crtc_id, uint64_t seq, uint64_t ts_ns) {
    drm::println("Page flip on CRTC {}: seq={}, timestamp={}ns", crtc_id, seq, ts_ns);
  });

  drm::println("\nAtomic modeset example complete.");
  drm::println("(Full modeset commit requires a framebuffer, which is not");
  drm::println("created in this example. See overlay_planes for allocator usage.)");

  return EXIT_SUCCESS;
}
