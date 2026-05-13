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
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/scene_set.hpp>

#include <drm.h>
#include <drm_fourcc.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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
  bool scene_test = false;
};

[[nodiscard]] Args parse_args(int argc, char* argv[]) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    if (a == "--device" && (i + 1) < argc) {
      args.device_path = argv[++i];
    } else if (a == "--hotplug") {
      args.hotplug = true;
    } else if (a == "--scene-test") {
      args.scene_test = true;
    } else if (a == "--help" || a == "-h") {
      drm::println("usage: {} [--device PATH] [--hotplug] [--scene-test]", argv[0]);
      std::exit(0);
    } else {
      drm::println(stderr, "unrecognized argument: {}", a);
      drm::println(stderr, "usage: {} [--device PATH] [--hotplug] [--scene-test]", argv[0]);
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

// Build N LayerScenes (one per connected output) carrying a single
// solid-color DumbBufferSource layer, hand them to SceneSet, and run
// one combined DRM_MODE_ATOMIC_TEST_ONLY commit. This exercises the
// SceneSet path on top of the same outputs the bare-kernel probe just
// validated — the verdict tells us the higher-level scene plumbing
// also lands a cross-CRTC atomic.
//
// Test-only by design: it never mutates kernel state visible on the
// physical display. Callers wanting a real flip should build a
// dual_display example instead.
void run_scene_test(drm::Device& dev,
                    const std::vector<drm::examples::multi_crtc::ConnectedOutput>& outputs) {
  if (outputs.empty()) {
    drm::println("scene-test: no outputs to drive");
    return;
  }

  std::vector<std::unique_ptr<drm::scene::LayerScene>> scenes;
  scenes.reserve(outputs.size());
  for (const auto& o : outputs) {
    drm::scene::LayerScene::Config cfg;
    cfg.crtc_id = o.crtc_id;
    cfg.connector_id = o.connector_id;
    cfg.mode = o.mode;
    auto s = drm::scene::LayerScene::create(dev, cfg);
    if (!s) {
      drm::println(stderr, "scene-test: LayerScene::create({}) failed: {}", o.connector_name,
                   s.error().message());
      return;
    }

    // One trivial full-size layer per scene so the allocator has
    // something to place. ARGB8888 dumb buffers are universally
    // accepted on PRIMARY planes; keep it small enough that
    // allocation never fails on production setups.
    auto src = drm::scene::DumbBufferSource::create(dev, o.mode.hdisplay, o.mode.vdisplay,
                                                    DRM_FORMAT_ARGB8888);
    if (!src) {
      drm::println(stderr, "scene-test: DumbBufferSource::create({}) failed: {}", o.connector_name,
                   src.error().message());
      return;
    }
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    desc.display.dst_rect = drm::scene::Rect{0, 0, o.mode.hdisplay, o.mode.vdisplay};
    if (auto h = (*s)->add_layer(std::move(desc)); !h) {
      drm::println(stderr, "scene-test: add_layer({}) failed: {}", o.connector_name,
                   h.error().message());
      return;
    }
    scenes.push_back(std::move(*s));
  }

  auto set_r = drm::scene::SceneSet::create(dev, std::move(scenes));
  if (!set_r) {
    drm::println(stderr, "scene-test: SceneSet::create failed: {}", set_r.error().message());
    return;
  }

  auto reports = (*set_r)->test();
  if (!reports) {
    drm::println("scene-test: SceneSet::test rejected: {} ({})", reports.error().message(),
                 reports.error().value());
    return;
  }
  drm::println("scene-test: SceneSet::test accepted ({} scene reports)", reports->size());
  for (std::size_t i = 0; i < reports->size(); ++i) {
    const auto& r = (*reports)[i];
    drm::println("  [{}] {} layers_total={} assigned={} composited={} dropped={}", i,
                 outputs[i].connector_name, r.layers_total, r.layers_assigned, r.layers_composited,
                 r.layers_unassigned);
  }
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
  if (args.scene_test) {
    auto outputs = drm::examples::multi_crtc::enumerate_connected_outputs(dev);
    run_scene_test(dev, outputs);
  }

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
    if (args.scene_test) {
      auto outputs = drm::examples::multi_crtc::enumerate_connected_outputs(dev);
      run_scene_test(dev, outputs);
    }
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
