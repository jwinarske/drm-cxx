// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// hotplug_monitor — drives a LayerScene that follows hotplug events.
//
// Usage: hotplug_monitor [/dev/dri/cardN]
//
// On startup the example finds the first connected connector with
// modes, brings up a LayerScene against that connector's preferred
// mode, and renders a four-quadrant test pattern + a small "clock
// badge" overlay (a coloured square whose hue advances each frame so
// you can see the example is alive).
//
// HotplugMonitor watches udev. Each event triggers a scan for the
// scene's "best" target connector + mode. If either changed the
// example calls `LayerScene::rebind` and continues running. Layer
// handles survive verbatim across the swap, so the test pattern and
// the clock badge follow the new mode without restart. The udev
// plumbing lives inside the library — this file just polls the fd.

#include "../common/format_probe.hpp"
#include "../common/open_output.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "display/hotplug_monitor.hpp"
#include "drm-cxx/detail/format.hpp"
#include "log.hpp"
#include "modeset/mode.hpp"
#include "scene/dumb_buffer_source.hpp"
#include "scene/layer_desc.hpp"
#include "scene/layer_handle.hpp"
#include "scene/layer_scene.hpp"
#include "session/seat.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <sys/poll.h>
#include <system_error>
#include <utility>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_quit = 0;

void signal_handler(int /*sig*/) {
  g_quit = 1;
}

// Pick the first connected connector with at least one mode and a
// usable encoder + CRTC. Returns the connector id, the resolved
// (CRTC id, mode) pair, or nullopt when nothing's lit.
struct ActiveConfig {
  std::uint32_t connector_id{0};
  std::uint32_t crtc_id{0};
  drmModeModeInfo mode{};
};

std::optional<ActiveConfig> find_active_config(int drm_fd) {
  const auto res = drm::get_resources(drm_fd);
  if (!res) {
    return std::nullopt;
  }
  for (int i = 0; i < res->count_connectors; ++i) {
    const auto conn = drm::get_connector(drm_fd, res->connectors[i]);
    if (!conn) {
      continue;
    }
    if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
      continue;
    }
    const auto modes = drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes);
    const auto pref = drm::select_preferred_mode(modes);
    if (!pref) {
      continue;
    }
    for (int e = 0; e < conn->count_encoders; ++e) {
      drmModeEncoderPtr enc = drmModeGetEncoder(drm_fd, conn->encoders[e]);
      if (enc == nullptr) {
        continue;
      }
      for (int c = 0; c < res->count_crtcs; ++c) {
        if ((enc->possible_crtcs & (1U << static_cast<unsigned>(c))) != 0) {
          ActiveConfig out;
          out.connector_id = conn->connector_id;
          out.crtc_id = res->crtcs[c];
          out.mode = pref->drm_mode;
          drmModeFreeEncoder(enc);
          return out;
        }
      }
      drmModeFreeEncoder(enc);
    }
  }
  return std::nullopt;
}

// Mode-equality check that ignores irrelevant tail fields. Two modes
// drive the same scanout when their hdisplay/vdisplay/clock match;
// flags / hsync / vsync rounding differences across kernel updates
// shouldn't trigger a rebind.
bool same_mode(const drmModeModeInfo& a, const drmModeModeInfo& b) {
  return a.hdisplay == b.hdisplay && a.vdisplay == b.vdisplay && a.clock == b.clock &&
         a.vrefresh == b.vrefresh;
}

// Paint a 4-quadrant test pattern (red / green / blue / white) into a
// linear ARGB8888 dumb buffer.
void paint_test_pattern(drm::scene::DumbBufferSource& src, std::uint32_t w, std::uint32_t h) {
  auto mapping = src.map(drm::MapAccess::Write);
  if (!mapping) {
    return;
  }
  const auto pixels = mapping->pixels();
  const std::uint32_t stride = mapping->stride();
  for (std::uint32_t y = 0; y < h; ++y) {
    auto* row =
        reinterpret_cast<std::uint32_t*>(pixels.data() + (static_cast<std::size_t>(y) * stride));
    for (std::uint32_t x = 0; x < w; ++x) {
      const bool right = x >= w / 2U;
      const bool bottom = y >= h / 2U;
      std::uint32_t px = 0xFFFF0000U;  // red
      if (right && !bottom) {
        px = 0xFF00FF00U;  // green
      } else if (!right && bottom) {
        px = 0xFF0000FFU;  // blue
      } else if (right && bottom) {
        px = 0xFFFFFFFFU;  // white
      }
      row[x] = px;
    }
  }
}

// Paint a flat colour into the badge buffer. The hue rotates each
// frame so the badge visibly pulses.
void paint_badge(drm::scene::DumbBufferSource& src, std::uint32_t w, std::uint32_t h,
                 std::uint32_t pixel) {
  auto mapping = src.map(drm::MapAccess::Write);
  if (!mapping) {
    return;
  }
  const auto pixels = mapping->pixels();
  const std::uint32_t stride = mapping->stride();
  for (std::uint32_t y = 0; y < h; ++y) {
    auto* row =
        reinterpret_cast<std::uint32_t*>(pixels.data() + (static_cast<std::size_t>(y) * stride));
    for (std::uint32_t x = 0; x < w; ++x) {
      row[x] = pixel;
    }
  }
}

constexpr std::uint32_t hue_color(std::uint32_t step) {
  // Cheap 6-step rainbow (red→yellow→green→cyan→blue→magenta→red…).
  // Avoids a real HSL→RGB just to give the badge a visual pulse.
  switch (step % 6U) {
    case 0:
      return 0xFFFF0000U;
    case 1:
      return 0xFFFFFF00U;
    case 2:
      return 0xFF00FF00U;
    case 3:
      return 0xFF00FFFFU;
    case 4:
      return 0xFF0000FFU;
    default:
      return 0xFFFF00FFU;
  }
}

void print_active(const ActiveConfig& cfg) {
  drm::println("  active: connector={} crtc={} mode={}x{}@{}Hz", cfg.connector_id, cfg.crtc_id,
               cfg.mode.hdisplay, cfg.mode.vdisplay, cfg.mode.vrefresh);
}

}  // namespace

int main(const int argc, char* argv[]) {
  // Connector-to-CRTC routing here uses `find_active_config` (above),
  // which walks `conn->encoders` + `possible_crtcs` rather than just
  // `conn->encoder_id`. That's the right shape for hotplug-time
  // re-resolution, so the initial pickup uses it too — only the device
  // open is shared with the rest of the example tree via open_device.
  auto ctx = drm::examples::open_device(argc, argv);
  if (!ctx) {
    return EXIT_FAILURE;
  }
  auto& dev = ctx->device;
  auto& seat = ctx->seat;

  auto active = find_active_config(dev.fd());
  if (!active) {
    drm::println(stderr, "No connected connector with a valid mode — nothing to drive");
    return EXIT_FAILURE;
  }
  drm::println("Initial config:");
  print_active(*active);

  drm::examples::warn_compat(drm::examples::probe_output(dev, active->crtc_id),
                             {.wants_alpha_overlays = true, .wants_explicit_zpos = true});

  // Build the scene against the initial configuration.
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = active->crtc_id;
  cfg.connector_id = active->connector_id;
  cfg.mode = active->mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::log_error("LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto& scene = **scene_r;

  // Test-pattern layer: full-screen, repainted on every rebind so it
  // matches the new mode's dimensions. The DumbBufferSource itself
  // has fixed dimensions, so we re-allocate via remove_layer +
  // add_layer when the mode changes (handle survival doesn't help if
  // the buffer's intrinsic size is wrong for the new mode).
  auto rebuild_test_pattern_layer =
      [&](std::uint32_t w,
          std::uint32_t h) -> drm::expected<drm::scene::LayerHandle, std::error_code> {
    auto src = drm::scene::DumbBufferSource::create(dev, w, h, DRM_FORMAT_ARGB8888);
    if (!src) {
      return drm::unexpected<std::error_code>(src.error());
    }
    paint_test_pattern(**src, w, h);
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = drm::scene::Rect{0, 0, w, h};
    desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
    desc.display.zpos = 1;
    return scene.add_layer(std::move(desc));
  };

  auto pattern_handle_r = rebuild_test_pattern_layer(active->mode.hdisplay, active->mode.vdisplay);
  if (!pattern_handle_r) {
    drm::log_error("test-pattern layer create: {}", pattern_handle_r.error().message());
    return EXIT_FAILURE;
  }
  drm::scene::LayerHandle pattern_handle = *pattern_handle_r;

  // Badge layer: a 96x96 coloured square in the top-left, repainted
  // each frame to advance its hue. Sized small enough to not need
  // resizing on rebind — the same buffer fits any mode the example
  // would drive.
  constexpr std::uint32_t k_badge_size = 96U;
  auto badge_src_r =
      drm::scene::DumbBufferSource::create(dev, k_badge_size, k_badge_size, DRM_FORMAT_ARGB8888);
  if (!badge_src_r) {
    drm::log_error("badge source create: {}", badge_src_r.error().message());
    return EXIT_FAILURE;
  }
  auto* badge_src_ptr = badge_src_r->get();
  paint_badge(**badge_src_r, k_badge_size, k_badge_size, hue_color(0));
  drm::scene::LayerDesc badge_desc;
  badge_desc.source = std::move(*badge_src_r);
  badge_desc.display.src_rect = drm::scene::Rect{0, 0, k_badge_size, k_badge_size};
  badge_desc.display.dst_rect = drm::scene::Rect{16, 16, k_badge_size, k_badge_size};
  badge_desc.display.zpos = 4;  // above amdgpu PRIMARY pin (zpos=2)
  auto badge_handle_r = scene.add_layer(std::move(badge_desc));
  if (!badge_handle_r) {
    drm::log_error("badge layer create: {}", badge_handle_r.error().message());
    return EXIT_FAILURE;
  }

  // First commit brings the CRTC up. Subsequent commits run on a
  // ~1Hz cadence to advance the badge hue.
  if (auto r = scene.commit(); !r) {
    drm::log_error("initial commit: {}", r.error().message());
    return EXIT_FAILURE;
  }

  drm::println("Monitoring hotplug events  (Ctrl-C to quit)");

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  auto monitor_res = drm::display::HotplugMonitor::open();
  if (!monitor_res) {
    drm::log_error("HotplugMonitor::open: {}", monitor_res.error().message());
    return EXIT_FAILURE;
  }
  auto& monitor = *monitor_res;

  // Re-evaluation logic shared by every hotplug event. Re-scans for
  // the best active config; if it differs from `active` we call
  // `rebind` and rebuild the test-pattern layer at the new size.
  // Connector reassignment (different connector becomes active) and
  // mode-list changes on the same connector both flow through here.
  auto on_hotplug = [&]() {
    const auto fresh = find_active_config(dev.fd());
    if (!fresh) {
      drm::println("[hotplug] no connected connector — scene paused");
      return;
    }
    const bool connector_changed = fresh->connector_id != active->connector_id;
    const bool crtc_changed = fresh->crtc_id != active->crtc_id;
    const bool mode_changed = !same_mode(fresh->mode, active->mode);
    if (!connector_changed && !crtc_changed && !mode_changed) {
      drm::println("[hotplug] no actionable change");
      return;
    }
    drm::println("[hotplug] rebinding scene:");
    print_active(*fresh);

    auto report = scene.rebind(fresh->crtc_id, fresh->connector_id, fresh->mode);
    if (!report) {
      drm::log_error("rebind: {}", report.error().message());
      return;
    }
    if (!report->empty()) {
      drm::println("  ({} layer(s) flagged incompatible — example continues anyway)",
                   report->incompatibilities.size());
    }

    // Test pattern was sized for the old mode. Re-create it at the
    // new dimensions and remove the old. The badge keeps its
    // existing source — it's mode-independent.
    scene.remove_layer(pattern_handle);
    auto new_pattern = rebuild_test_pattern_layer(fresh->mode.hdisplay, fresh->mode.vdisplay);
    if (!new_pattern) {
      drm::log_error("rebind test-pattern layer: {}", new_pattern.error().message());
      return;
    }
    pattern_handle = *new_pattern;

    if (auto r = scene.commit(); !r) {
      drm::log_error("post-rebind commit: {}", r.error().message());
      return;
    }
    active = fresh;
  };

  monitor.set_handler([&](const drm::display::HotplugEvent& ev) {
    drm::println("\n[hotplug] devnode={}{}{}", ev.devnode.empty() ? "(none)" : ev.devnode,
                 ev.connector_id ? " connector=" : "",
                 ev.connector_id ? std::to_string(*ev.connector_id) : "");
    on_hotplug();
  });

  pollfd pfds[2]{};
  pfds[0].fd = monitor.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = seat ? seat->poll_fd() : -1;
  pfds[1].events = POLLIN;

  std::uint32_t hue_step = 1;
  while (g_quit == 0) {
    if (const int ret = poll(pfds, 2, /*timeout_ms=*/1000); ret < 0) {
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
    // Repaint the badge each second-ish loop iteration.
    paint_badge(*badge_src_ptr, k_badge_size, k_badge_size, hue_color(hue_step));
    ++hue_step;
    if (auto r = scene.commit(); !r) {
      // EACCES is the seat telling us we lost master — the scene's
      // own suspended_ flag will short-circuit subsequent commits
      // until on_session_resumed clears it. Don't spam the log.
      if (r.error().value() != EACCES) {
        drm::log_warn("commit: {}", r.error().message());
      }
    }
  }

  drm::println("\nShutting down...");
  return EXIT_SUCCESS;
}
