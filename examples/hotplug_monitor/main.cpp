// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// hotplug_monitor — tiny demo of drm::display::HotplugMonitor.
//
// Usage: hotplug_monitor [/dev/dri/cardN]
//
// All the udev plumbing (netlink socket, subsystem filtering, HOTPLUG=1
// gating, CONNECTOR=<id> parsing) lives inside the library — this file
// just wires `HotplugMonitor::fd()` into a poll loop and pretty-prints
// each event.

#include "../select_device.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "display/hotplug_monitor.hpp"
#include "drm-cxx/detail/format.hpp"
#include "log.hpp"
#include "modeset/mode.hpp"
#include "session/seat.hpp"

#include <drm-cxx/detail/span.hpp>

#include <xf86drmMode.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <sys/poll.h>
#include <system_error>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_quit = 0;

void signal_handler(int /*sig*/) {
  g_quit = 1;
}

void print_one_connector(const int drm_fd, const uint32_t connector_id) {
  const auto conn = drm::get_connector(drm_fd, connector_id);
  if (!conn) {
    drm::log_error("get_connector({}) failed", connector_id);
    return;
  }

  const char* status = "unknown";
  switch (conn->connection) {
    case DRM_MODE_CONNECTED:
      status = "connected";
      break;
    case DRM_MODE_DISCONNECTED:
      status = "disconnected";
      break;
    default:
      break;
  }

  drm::println("    connector-{}: {} ({} modes)", conn->connector_id, status, conn->count_modes);
  if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
    const auto modes = drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes);
    if (const auto pref = drm::select_preferred_mode(modes)) {
      drm::println("      preferred: {}x{}@{}Hz", pref->width(), pref->height(), pref->refresh());
    }
  }
}

void print_connector_status(const int drm_fd) {
  const auto res = drm::get_resources(drm_fd);
  if (!res) {
    drm::log_error("Failed to get DRM resources");
    return;
  }
  drm::println("  Connectors ({}):", res->count_connectors);
  for (int i = 0; i < res->count_connectors; ++i) {
    drm::println("    [{}]:", i);
    print_one_connector(drm_fd, res->connectors[i]);
  }
}

}  // namespace

int main(const int argc, char* argv[]) {
  const auto path = drm::examples::select_device(argc, argv);
  if (!path) {
    return EXIT_FAILURE;
  }

  // See atomic_modeset for why we claim a seat session.
  auto seat = drm::session::Seat::open();

  auto dev_result = drm::Device::open(*path);
  if (!dev_result) {
    drm::println(stderr, "Failed to open {}", *path);
    return EXIT_FAILURE;
  }
  const auto& dev = *dev_result;

  drm::println("Monitoring hotplug events on {}  (Ctrl-C to quit)", *path);
  drm::println("Current state:");
  print_connector_status(dev.fd());

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  auto monitor_res = drm::display::HotplugMonitor::open();
  if (!monitor_res) {
    drm::log_error("HotplugMonitor::open failed: {}", monitor_res.error().message());
    return EXIT_FAILURE;
  }
  auto& monitor = *monitor_res;
  monitor.set_handler([fd = dev.fd()](const drm::display::HotplugEvent& ev) {
    drm::println("\n[hotplug] devnode={}{}{}", ev.devnode.empty() ? "(none)" : ev.devnode,
                 ev.connector_id ? " connector=" : "",
                 ev.connector_id ? std::to_string(*ev.connector_id) : "");
    if (ev.connector_id) {
      print_one_connector(fd, *ev.connector_id);
    } else {
      drm::println("  (no CONNECTOR hint — re-enumerating all)");
      print_connector_status(fd);
    }
  });

  pollfd pfds[2]{};
  pfds[0].fd = monitor.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = seat ? seat->poll_fd() : -1;
  pfds[1].events = POLLIN;

  while (g_quit == 0) {
    if (const int ret = poll(pfds, 2, 100); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::log_error("poll: {} ({})", std::system_category().message(errno), errno);
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      if (auto r = monitor.dispatch(); !r) {
        drm::log_error("HotplugMonitor::dispatch: {}", r.error().message());
        break;
      }
    }
    if ((pfds[1].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }
  }

  drm::println("\nShutting down...");
  return EXIT_SUCCESS;
}