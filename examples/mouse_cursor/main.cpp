// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// mouse_cursor — shows a cursor that tracks the mouse via libinput.
//
// Usage: mouse_cursor [--theme NAME] [--cursor NAME] [--size N]
//                    [--plane ID] [/dev/dri/cardN]
//
// Loads a cursor from an installed XCursor theme (Adwaita by default)
// and tracks the mouse via libinput. The drm::cursor::Renderer picks
// the best KMS path available on the chosen CRTC (dedicated CURSOR
// plane → atomic OVERLAY with ARGB8888 → legacy drmModeSetCursor);
// the log line at startup reports which it chose. Press Escape or
// Ctrl-C to quit. Middle-click or digit keys 1..9 cycle through the
// shape set.
//
// --plane ID forces the renderer to use a specific plane id — useful
// for exercising a non-default path on a platform that advertises
// several overlays.

#include "../common/open_output.hpp"
#include "../common/vt_switch.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "cursor/cursor.hpp"
#include "cursor/renderer.hpp"
#include "cursor/theme.hpp"
#include "drm-cxx/detail/format.hpp"
#include "input/pointer.hpp"
#include "input/seat.hpp"
#include "session/seat.hpp"

#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <optional>
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

// Shapes that middle-click cycles through and digit keys 1..9 jump to.
// Covers the common intentional shapes (pointers, text, wait) plus a
// couple that illustrate different hotspots (crosshair, grabbing) and
// animation (wait, progress).
constexpr std::array<const char*, 9> k_cycle = {
    "default", "pointer",  "text",        "crosshair", "help",
    "wait",    "progress", "not-allowed", "grabbing",
};

const char* path_name(const drm::cursor::PlanePath p) {
  switch (p) {
    case drm::cursor::PlanePath::kAtomicCursor:
      return "atomic CURSOR plane";
    case drm::cursor::PlanePath::kAtomicOverlay:
      return "atomic OVERLAY plane";
    case drm::cursor::PlanePath::kLegacy:
      return "legacy drmModeSetCursor";
  }
  return "unknown";
}

// Parse a positive integer with overflow detection. strtol's errno
// contract is what distinguishes "99999999999" (ERANGE) from atoi's
// silent UB.
bool parse_uint(const char* s, const int max, int& out) {
  if (s == nullptr || *s == '\0') {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v < 0 || v > max) {
    return false;
  }
  out = static_cast<int>(v);
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  // ---------------------------------------------------------------------------
  // CLI parse. Pre-strip our flags so select_device only sees the
  // optional device path.
  // ---------------------------------------------------------------------------
  const char* cli_theme = nullptr;
  const char* cli_cursor = "default";
  int cli_size = 0;   // 0 ⇒ let renderer pick via DRM_CAP_CURSOR_WIDTH
  int cli_plane = 0;  // 0 ⇒ let renderer pick via PlaneRegistry

  auto strip = [&](int i, int n) {
    for (int j = i; j + n < argc; ++j) {
      argv[j] = argv[j + n];
    }
    argc -= n;
  };

  for (int i = 1; i < argc;) {
    const bool is_theme = std::strcmp(argv[i], "--theme") == 0;
    const bool is_cursor = std::strcmp(argv[i], "--cursor") == 0;
    const bool is_size = std::strcmp(argv[i], "--size") == 0;
    const bool is_plane = std::strcmp(argv[i], "--plane") == 0;
    if (is_theme || is_cursor || is_size || is_plane) {
      if (i + 1 >= argc) {
        drm::println(stderr, "{}: missing value", argv[i]);
        return EXIT_FAILURE;
      }
      if (is_theme) {
        cli_theme = argv[i + 1];
      } else if (is_cursor) {
        cli_cursor = argv[i + 1];
      } else if (is_size) {
        if (!parse_uint(argv[i + 1], 4096, cli_size)) {
          drm::println(stderr, "--size: invalid value '{}' (expected 0..4096)", argv[i + 1]);
          return EXIT_FAILURE;
        }
      } else if (!parse_uint(argv[i + 1], 0x7fffffff, cli_plane)) {
        drm::println(stderr, "--plane: invalid value '{}'", argv[i + 1]);
        return EXIT_FAILURE;
      }
      strip(i, 2);
      continue;
    }
    ++i;
  }

  // ---------------------------------------------------------------------------
  // Device + libseat. The example uses the CRTC's *current* mode for
  // pointer clamping (not the connector's preferred mode), so it can't
  // share open_and_pick_output's preferred-mode pickup — only the open
  // half is shared.
  // ---------------------------------------------------------------------------
  auto ctx = drm::examples::open_device(argc, argv);
  if (!ctx) {
    return EXIT_FAILURE;
  }
  auto& dev = ctx->device;
  auto& seat = ctx->seat;

  // ---------------------------------------------------------------------------
  // CRTC discovery — first connected connector with an active mode.
  // Renderer needs the CRTC id; the mode dimensions are still our job
  // (they feed pointer clamping, not the renderer itself).
  // ---------------------------------------------------------------------------
  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    drm::println(stderr, "Failed to get DRM resources");
    return EXIT_FAILURE;
  }

  std::uint32_t crtc_id = 0;
  std::uint32_t mode_w = 0;
  std::uint32_t mode_h = 0;
  for (int i = 0; i < res->count_connectors; ++i) {
    auto conn = drm::get_connector(dev.fd(), res->connectors[i]);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0 ||
        conn->encoder_id == 0) {
      continue;
    }
    auto enc = drm::get_encoder(dev.fd(), conn->encoder_id);
    if (!enc || enc->crtc_id == 0) {
      continue;
    }
    auto crtc = drm::get_crtc(dev.fd(), enc->crtc_id);
    if (!crtc || crtc->mode_valid == 0) {
      continue;
    }
    crtc_id = enc->crtc_id;
    mode_w = crtc->mode.hdisplay;
    mode_h = crtc->mode.vdisplay;
    drm::println("Using CRTC {} ({}x{})", crtc_id, mode_w, mode_h);
    break;
  }
  if (crtc_id == 0) {
    drm::println(stderr, "No active CRTC found");
    return EXIT_FAILURE;
  }

  // ---------------------------------------------------------------------------
  // Theme + cursor load.
  // ---------------------------------------------------------------------------
  auto theme = drm::cursor::Theme::discover();
  if (!theme) {
    drm::println(stderr, "No XCursor themes found on system search path");
    return EXIT_FAILURE;
  }

  // Target cursor size: --size wins; otherwise query the HW cap so an
  // atomic CURSOR plane gets its natively supported dimensions.
  std::uint32_t target_size = (cli_size > 0) ? static_cast<std::uint32_t>(cli_size) : 0U;
  if (target_size == 0) {
    std::uint64_t cap_w = 0;
    drmGetCap(dev.fd(), DRM_CAP_CURSOR_WIDTH, &cap_w);
    target_size = (cap_w != 0) ? static_cast<std::uint32_t>(cap_w) : 64U;
  }

  const std::string_view theme_hint =
      (cli_theme != nullptr) ? std::string_view(cli_theme) : std::string_view{};
  auto initial_cursor = drm::cursor::Cursor::load(*theme, cli_cursor, theme_hint, target_size);
  if (!initial_cursor) {
    drm::println(stderr, "Failed to load cursor '{}' (theme hint '{}', size {})", cli_cursor,
                 cli_theme != nullptr ? cli_theme : "(default)", target_size);
    return EXIT_FAILURE;
  }
  bool animated = initial_cursor->animated();
  {
    const auto& f = initial_cursor->first();
    drm::println("Cursor '{}' loaded: {}x{}, hotspot ({}, {}){}", cli_cursor, f.width, f.height,
                 f.xhot, f.yhot, animated ? " [animated]" : "");
  }

  // ---------------------------------------------------------------------------
  // Renderer.
  // ---------------------------------------------------------------------------
  drm::cursor::RendererConfig cfg;
  cfg.crtc_id = crtc_id;
  cfg.preferred_size = target_size;
  cfg.forced_plane_id = static_cast<std::uint32_t>(cli_plane);
  auto renderer_result = drm::cursor::Renderer::create(dev, cfg);
  if (!renderer_result) {
    drm::println(stderr, "Renderer::create failed: {}", renderer_result.error().message());
    return EXIT_FAILURE;
  }
  auto& renderer = *renderer_result;
  drm::println("Cursor path: {} (plane {})", path_name(renderer.path()), renderer.plane_id());

  if (auto r = renderer.set_cursor(std::move(*initial_cursor)); !r) {
    drm::println(stderr, "set_cursor failed: {}", r.error().message());
    return EXIT_FAILURE;
  }

  // Match the initial cursor against k_cycle so the first middle-click
  // advances to the next entry rather than restarting from index 0.
  std::size_t current_idx = 0;
  for (std::size_t i = 0; i < k_cycle.size(); ++i) {
    if (std::strcmp(k_cycle.at(i), cli_cursor) == 0) {
      current_idx = i;
      break;
    }
  }

  // Shape cycling closure — shared by middle-click and digit-key paths.
  auto load_and_apply = [&](std::size_t idx) {
    auto next = drm::cursor::Cursor::load(*theme, k_cycle.at(idx), theme_hint, target_size);
    if (!next) {
      drm::println(stderr, "Cursor '{}' not found in any theme", k_cycle.at(idx));
      return;
    }
    const bool next_animated = next->animated();
    const auto f = next->first();  // snapshot before move
    if (auto r = renderer.set_cursor(std::move(*next)); !r) {
      drm::println(stderr, "set_cursor failed: {}", r.error().message());
      return;
    }
    drm::println("Cursor: {} ({}x{}, hotspot ({}, {})){}", k_cycle.at(idx), f.width, f.height,
                 f.xhot, f.yhot, next_animated ? " [animated]" : "");
    animated = next_animated;
    current_idx = idx;
  };

  // ---------------------------------------------------------------------------
  // Input seat (libinput via libseat when available).
  // ---------------------------------------------------------------------------
  drm::input::InputDeviceOpener input_opener;
  if (seat) {
    input_opener = seat->input_opener();
  }
  auto input_seat_result = drm::input::Seat::open({}, std::move(input_opener));
  if (!input_seat_result) {
    drm::println(stderr, "Failed to open input seat (need root or input group)");
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_result;

  drm::input::Pointer pointer;
  pointer.reset_position(static_cast<double>(mode_w) / 2.0, static_cast<double>(mode_h) / 2.0);

  bool cursor_dirty = false;

  drm::examples::VtChordTracker vt_chord;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event);
        ke != nullptr && vt_chord.observe(*ke, seat ? &*seat : nullptr)) {
      return;
    }
    if (const auto* pe = std::get_if<drm::input::PointerEvent>(&event)) {
      if (const auto* m = std::get_if<drm::input::PointerMotionEvent>(pe)) {
        pointer.accumulate_motion(m->dx, m->dy);
        cursor_dirty = true;
      } else if (const auto* b = std::get_if<drm::input::PointerButtonEvent>(pe)) {
        pointer.set_button(b->button, b->pressed);
        if (b->pressed) {
          if (b->button == BTN_MIDDLE) {
            load_and_apply((current_idx + 1) % k_cycle.size());
          } else {
            drm::println("Button 0x{:x} at ({:.0f}, {:.0f})", b->button, pointer.x(), pointer.y());
          }
        }
      }
    }
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (vt_chord.is_quit_key(*ke)) {
        g_quit = 1;
      } else if (ke->pressed && ke->key >= KEY_1 && ke->key <= KEY_9) {
        const auto digit = static_cast<std::size_t>(ke->key - KEY_1);
        load_and_apply(std::min(digit, k_cycle.size() - 1));
      }
    }
  });

  // ---------------------------------------------------------------------------
  // Seat pause/resume. The renderer handles its own buffer + property
  // rebuild against a fresh fd, but DRM client caps (UNIVERSAL_PLANES,
  // ATOMIC) are per-fd kernel state — they don't survive across the
  // libseat fd swap, so the new fd needs them re-enabled before the
  // renderer's atomic-path commit will succeed (otherwise EINVAL and
  // the cursor never reappears post-resume).
  //
  // We defer the actual rebuild out of the libseat callback into the
  // main loop, both so the work is short-lived inside the listener and
  // so the post-rebuild commit can be re-attempted on the next
  // iteration if it transiently fails (drmIsMaster lag — see
  // reference_drmismaster_lag.md).
  // ---------------------------------------------------------------------------
  int pending_resume_fd = -1;
  if (seat) {
    seat->set_pause_callback([&]() {
      renderer.on_session_paused();
      (void)input_seat.suspend();
    });
    seat->set_resume_callback([&](std::string_view /*path*/, int new_fd) {
      pending_resume_fd = new_fd;
      (void)input_seat.resume();
    });
  }

  // Initial cursor install at the pointer's starting position.
  if (auto r = renderer.move_to(static_cast<int>(pointer.x()), static_cast<int>(pointer.y())); !r) {
    drm::println(stderr, "Initial move_to failed: {}", r.error().message());
    return EXIT_FAILURE;
  }

  drm::println("Cursor active ({}x{}) — move mouse, middle-click or 1-9 to cycle, Escape to quit",
               mode_w, mode_h);

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // ---------------------------------------------------------------------------
  // Main loop.
  // ---------------------------------------------------------------------------
  pollfd pfds[2]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = seat ? seat->poll_fd() : -1;
  pfds[1].events = POLLIN;

  while (g_quit == 0) {
    // Animated cursors need a shorter poll so tick() can step frames
    // at roughly refresh cadence. Idle (static) cadence stays coarse
    // to minimize wake-ups.
    const int poll_timeout = animated ? 16 : 100;
    if (const int ret = poll(pfds, 2, poll_timeout); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      if (auto r = input_seat.dispatch(); !r) {
        drm::println(stderr, "input dispatch failed");
        break;
      }
    }
    if ((pfds[1].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }

    if (pending_resume_fd != -1) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      // Replace the stale Device wrapper so caps are enabled on the
      // fresh fd; without this the renderer's atomic commit below
      // fails with EINVAL on the new fd. dev is a reference into ctx,
      // so move-assigning ctx->device updates the same underlying
      // Device the renderer will read through new_fd.
      ctx->device = drm::Device::from_fd(new_fd);
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed: {}", r.error().message());
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed: {}", r.error().message());
        break;
      }
      if (auto r = renderer.on_session_resumed(new_fd); !r) {
        drm::println(stderr, "Renderer resume failed: {}", r.error().message());
        // Don't bail — force a retry on the next move via cursor_dirty
        // in case the failure was the drmIsMaster lag window.
      }
      cursor_dirty = true;
    }

    // Animated cursors advance the frame via tick(); the renderer
    // re-commits only when the selected frame changes.
    if (animated) {
      (void)renderer.tick();
    }

    // Commit motion once per loop iteration, after all pending input
    // has drained — mice can report >1000 events/sec, and we don't
    // want one atomic commit per motion delta.
    if (cursor_dirty) {
      const double cx = std::clamp(pointer.x(), 0.0, static_cast<double>(mode_w - 1));
      const double cy = std::clamp(pointer.y(), 0.0, static_cast<double>(mode_h - 1));
      pointer.reset_position(cx, cy);
      if (auto r = renderer.move_to(static_cast<int>(cx), static_cast<int>(cy)); !r) {
        drm::println(stderr, "move_to failed: {}", r.error().message());
        break;
      }
      cursor_dirty = false;
    }
  }

  // Renderer destructor detaches the cursor from its plane and frees
  // the dumb buffer; no manual teardown needed.
  return EXIT_SUCCESS;
}
