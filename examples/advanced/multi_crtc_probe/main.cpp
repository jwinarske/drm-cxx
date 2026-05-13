// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// multi_crtc_probe — list connected outputs on a DRM card and report
// whether the kernel will accept a TEST_ONLY commit that touches every
// listed CRTC at once. This is the foundational signal a SceneSet-style
// coordinator needs: combined commits are the only path to tear-free
// synchronized changes across multiple displays.
//
// Usage:
//   multi_crtc_probe [--device PATH] [--hotplug]
//
//   --device PATH   DRM node, default /dev/dri/card0
//   --hotplug       Install drm::display::HotplugMonitor and re-probe
//                   on every connector add/remove event. Press 'q' or
//                   ctrl-c to exit. Without this the probe runs once
//                   against the current topology and exits.
//
// Exit codes:
//   0   probe ran (verdict may be Accepted, Rejected, or NotApplicable)
//   1   could not open the DRM device, or probe failed at the
//       resource-allocation step (scratch dumb buffer / Modeset /
//       property cache).

#include "../../common/multi_crtc_probe.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/hotplug_monitor.hpp>

#include <drm.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_sigint_received = 0;

void sigint_handler(int /*sig*/) {
  g_sigint_received = 1;
}

void install_sigint_handler() {
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);
}

struct Args {
  std::string device_path = "/dev/dri/card0";
  bool hotplug = false;
};

[[nodiscard]] Args parse_args(int argc, char* argv[]) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    if (a == "--device" && (i + 1) < argc) {
      args.device_path = argv[++i];
    } else if (a == "--hotplug") {
      args.hotplug = true;
    } else if (a == "--help" || a == "-h") {
      drm::println("usage: {} [--device PATH] [--hotplug]", argv[0]);
      std::exit(0);
    } else {
      drm::println(stderr, "unrecognized argument: {}", a);
      drm::println(stderr, "usage: {} [--device PATH] [--hotplug]", argv[0]);
      std::exit(2);
    }
  }
  return args;
}

void print_outputs(const std::vector<drm::examples::multi_crtc::ConnectedOutput>& outputs) {
  if (outputs.empty()) {
    drm::println("connected outputs: (none)");
    return;
  }
  drm::println("connected outputs: {}", outputs.size());
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    const auto& o = outputs[i];
    drm::println("  [{}] {} connector={} crtc={} primary_plane={} mode={}x{}@{:.2f}Hz", i,
                 o.connector_name, o.connector_id, o.crtc_id, o.primary_plane_id, o.mode.hdisplay,
                 o.mode.vdisplay, static_cast<double>(o.mode.vrefresh));
  }
}

void print_report(const drm::examples::multi_crtc::ProbeReport& report) {
  using V = drm::examples::multi_crtc::CombinedAtomicVerdict;
  drm::println("combined-atomic probe: {} (outputs_in_probe={})",
               drm::examples::multi_crtc::to_string(report.verdict), report.outputs_in_probe);
  if (report.verdict == V::Rejected && report.error) {
    drm::println("  kernel rejected: {} ({})", report.error.message(), report.error.value());
  }
}

void run_probe(const drm::Device& dev) {
  auto outputs = drm::examples::multi_crtc::enumerate_connected_outputs(dev);
  print_outputs(outputs);
  auto report = drm::examples::multi_crtc::probe_combined_atomic(
      dev, drm::span<const drm::examples::multi_crtc::ConnectedOutput>{outputs});
  print_report(report);
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto args = parse_args(argc, argv);

  auto dev_r = drm::Device::open(args.device_path);
  if (!dev_r) {
    drm::println(stderr, "drm::Device::open({}): {}", args.device_path, dev_r.error().message());
    return 1;
  }
  auto dev = std::move(*dev_r);
  if (auto r = dev.enable_universal_planes(); !r) {
    drm::println(stderr,
                 "enable_universal_planes: {} (some platforms don't permit this; "
                 "plane enumeration will be empty)",
                 r.error().message());
  }
  if (auto r = dev.set_client_cap(DRM_CLIENT_CAP_ATOMIC, 1); !r) {
    drm::println(stderr, "DRM_CLIENT_CAP_ATOMIC: {} (atomic-only TEST commits will fail)",
                 r.error().message());
  }
  drm::println("device: {} (fd={})", args.device_path, dev.fd());

  run_probe(dev);

  if (!args.hotplug) {
    return 0;
  }

  // --hotplug: install monitor + poll loop, re-probe on each event,
  // exit on SIGINT or 'q' on stdin.
  auto monitor_r = drm::display::HotplugMonitor::open();
  if (!monitor_r) {
    drm::println(stderr, "HotplugMonitor::open failed: {}", monitor_r.error().message());
    return 1;
  }
  auto monitor = std::move(*monitor_r);
  monitor.set_handler([&](const drm::display::HotplugEvent& ev) {
    drm::println("");
    drm::println("hotplug event (devnode={}, connector_id={})", ev.devnode,
                 ev.connector_id.value_or(0));
    run_probe(dev);
  });

  install_sigint_handler();
  drm::println("");
  drm::println("watching for hotplug events; press ctrl-c or 'q'+enter to exit");

  std::array<pollfd, 2> fds{
      pollfd{.fd = monitor.fd(), .events = POLLIN, .revents = 0},
      pollfd{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
  };
  while (g_sigint_received == 0) {
    const int n = ::poll(fds.data(), fds.size(), -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::strerror(errno));
      break;
    }
    if ((fds[0].revents & POLLIN) != 0) {
      if (auto r = monitor.dispatch(); !r) {
        drm::println(stderr, "hotplug dispatch: {}", r.error().message());
      }
    }
    if ((fds[1].revents & POLLIN) != 0) {
      char buf[64];
      const ssize_t n_read = ::read(STDIN_FILENO, buf, sizeof(buf));
      if (n_read > 0 && (buf[0] == 'q' || buf[0] == 'Q')) {
        break;
      }
    }
  }

  return 0;
}
