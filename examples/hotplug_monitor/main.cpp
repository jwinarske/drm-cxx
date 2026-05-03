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
#include "../common/vt_switch.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "display/hotplug_monitor.hpp"
#include "drm-cxx/detail/format.hpp"
#include "input/seat.hpp"
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
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <variant>

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
// shouldn't trigger a `rebind`.
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

// Paint a flat color into the badge buffer. The hue rotates each
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
  // 6-step rainbow (red→yellow→green→cyan→blue→magenta→red…).
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

  // Test-pattern layer: full-screen, repainted on every `rebind` so it
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

  // Badge layer: a 96x96 colored square in the top-left, repainted
  // each frame to advance its hue. Sized small enough to not need
  // resizing on `rebind` — the same buffer fits any mode the example
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

  // The first commit brings the CRTC up. Subsequent commits run on a
  // ~1Hz cadence to advance the badge hue.
  if (auto r = scene.commit(); !r) {
    drm::log_error("initial commit: {}", r.error().message());
    return EXIT_FAILURE;
  }

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // libinput keyboard: Esc/q quits and Ctrl+Alt+F<n> switches VT. The
  // signal handlers above are only effective when stdin is still a
  // line-discipline TTY — once libseat puts the seat into KD_GRAPHICS
  // the kernel suppresses Ctrl-C signal generation, so on a real VT
  // this is the only reliable exit path.
  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_res = drm::input::Seat::open({}, std::move(libinput_opener));
  std::optional<drm::input::Seat> input_seat;
  drm::examples::VtChordTracker vt_chord;
  if (input_seat_res) {
    input_seat = std::move(*input_seat_res);
    input_seat->set_event_handler([&](const drm::input::InputEvent& event) {
      const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event);
      if (ke == nullptr) {
        return;
      }
      if (vt_chord.observe(*ke, seat ? &*seat : nullptr)) {
        return;
      }
      if (vt_chord.is_quit_key(*ke)) {
        g_quit = 1;
      }
    });
    drm::println("Monitoring hotplug events  (Esc/q or Ctrl-C to quit)");
  } else {
    drm::println(stderr,
                 "input::Seat::open: {} — Esc/q exit unavailable (need 'input' group or a seat "
                 "backend); Ctrl-C only works on a non-graphics TTY",
                 input_seat_res.error().message());
    drm::println("Monitoring hotplug events  (Ctrl-C to quit)");
  }

  // Seat pause/resume. The fd-bound half (plane registry, property
  // ids, MODE_ID blob, per-source FB handles) is rebuilt by
  // scene.on_session_resumed against the new Device; client caps
  // (UNIVERSAL_PLANES, ATOMIC) are per-fd and need re-enabling here.
  // The actual rebuild runs from the main loop, not the libseat
  // callback, so a transient EACCES at handover gets retried.
  bool session_paused = false;
  int pending_resume_fd = -1;
  bool resume_retry = false;
  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      scene.on_session_paused();
      if (input_seat) {
        (void)input_seat->suspend();
      }
    });
    seat->set_resume_callback([&](std::string_view /*path*/, int new_fd) {
      pending_resume_fd = new_fd;
      session_paused = false;
      if (input_seat) {
        (void)input_seat->resume();
      }
    });
  }

  auto monitor_res = drm::display::HotplugMonitor::open();
  if (!monitor_res) {
    drm::log_error("HotplugMonitor::open: {}", monitor_res.error().message());
    return EXIT_FAILURE;
  }
  auto& monitor = *monitor_res;

  // Re-evaluation logic shared by every hotplug event. Re-scans for
  // the best active config; if it differs from `active`, we call
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

  pollfd pfds[3]{};
  pfds[0].fd = monitor.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = seat ? seat->poll_fd() : -1;
  pfds[1].events = POLLIN;
  pfds[2].fd = input_seat ? input_seat->fd() : -1;
  pfds[2].events = POLLIN;

  std::uint32_t hue_step = 1;
  while (g_quit == 0) {
    // Block while paused so the badge timer doesn't spin a free-running
    // loop — wake on monitor / seat / input fds only.
    const int timeout_ms = session_paused ? -1 : 1000;
    if (const int ret = poll(pfds, 3, timeout_ms); ret < 0) {
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
    if (input_seat && (pfds[2].revents & POLLIN) != 0) {
      if (auto r = input_seat->dispatch(); !r) {
        drm::log_error("input dispatch: {}", r.error().message());
      }
    }

    // Two-stage resume: when pending_resume_fd is set we own the new
    // libseat fd and need to take it (Device::from_fd is owning, so we
    // must do this exactly once per fd). After the swap, if the scene
    // rebuild fails (typically a transient drmIsMaster lag), we leave
    // resume_retry set so the next loop iteration retries
    // on_session_resumed against the already-installed dev — without
    // re-calling from_fd, which would double-close the fd.
    if (pending_resume_fd != -1) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      // ctx->device is captured by `dev` reference; move-assigning here
      // updates the same underlying Device the scene will read through.
      ctx->device = drm::Device::from_fd(new_fd);
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::log_error("resume: enable_universal_planes failed: {}", r.error().message());
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::log_error("resume: enable_atomic failed: {}", r.error().message());
        break;
      }
      resume_retry = true;
    }
    if (resume_retry) {
      if (auto r = scene.on_session_resumed(dev); !r) {
        drm::log_error("resume: on_session_resumed failed: {}", r.error().message());
        session_paused = true;
        continue;
      }
      resume_retry = false;
    }

    if (session_paused) {
      continue;
    }

    // Repaint the badge each second-ish loop iteration.
    paint_badge(*badge_src_ptr, k_badge_size, k_badge_size, hue_color(hue_step));
    ++hue_step;
    if (auto r = scene.commit(); !r) {
      // EACCES is the seat telling us we lost master — the scene's
      // own suspended_ flag will short-circuit later `commits`
      // until on_session_resumed clears it. Don't spam the log.
      if (r.error().value() != EACCES) {
        drm::log_warn("commit: {}", r.error().message());
      }
    }
  }

  drm::println("\nShutting down...");
  return EXIT_SUCCESS;
}
