// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cursor_rotate — hardware validation harness for drm::cursor V2.
//
// Usage: cursor_rotate [--theme NAME] [--cursor NAME] [--size N]
//                     [--period MS] [--no-rotate] [/dev/dri/cardN]
//
// Exit: Esc or q (via libinput) or Ctrl-C. The libinput path is the
// only reliable one on a real VT — when libseat puts the TTY into
// KD_GRAPHICS the kernel suppresses Ctrl-C signal generation. The
// same input source carries Ctrl+Alt+F<n> for VT switching.
//
// What this example validates on real hardware:
//
//   1. Theme resolve cache. Two back-to-back resolve() calls for the
//      same (name, theme) pair are timed; the second should land in
//      the memoization cache and come out in nanoseconds.
//
//   2. HOTSPOT_X / HOTSPOT_Y plane properties. has_hotspot_properties()
//      is printed per-renderer. Expected true on virtualized guests
//      (virtio-gpu, vmwgfx) and false on bare metal. Inside a VM,
//      verify the host's native mouse cursor aligns with the guest
//      sprite's tip — without these properties the host sees the
//      sprite's top-left corner and misaligns by ~xhot pixels.
//
//   3. Shared Cursor across multi-CRTC Renderers. One Cursor::load is
//      performed; the result is wrapped in shared_ptr<const Cursor>
//      and handed to every active CRTC's Renderer. Pixel storage is
//      not duplicated.
//
//   4. set_rotation() runtime setter. Unless --no-rotate is passed,
//      the example cycles through k0 → k90 → k180 → k270 every
//      --period milliseconds. Each change is committed via an atomic
//      plane commit; the sprite visibly rotates on screen.
//
//   5. Software pre-rotation for planes without the rotation prop.
//      has_hardware_rotation() is printed per-renderer. On bare-metal
//      planes that don't expose the rotation property (common on
//      older GPUs and embedded stacks) it's false — the rotation
//      cycle still works, and blit_frame does the work on the CPU.

#include "../common/open_output.hpp"
#include "../common/vt_switch.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "cursor/cursor.hpp"
#include "cursor/renderer.hpp"
#include "cursor/theme.hpp"
#include "drm-cxx/detail/format.hpp"
#include "input/seat.hpp"

#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_quit = 0;

void signal_handler(int /*sig*/) {
  g_quit = 1;
}

const char* path_name(drm::cursor::PlanePath p) {
  switch (p) {
    case drm::cursor::PlanePath::kAtomicCursor:
      return "atomic CURSOR";
    case drm::cursor::PlanePath::kAtomicOverlay:
      return "atomic OVERLAY";
    case drm::cursor::PlanePath::kLegacy:
      return "legacy SetCursor";
  }
  return "unknown";
}

const char* rotation_name(drm::cursor::Rotation r) {
  switch (r) {
    case drm::cursor::Rotation::k0:
      return "0";
    case drm::cursor::Rotation::k90:
      return "90";
    case drm::cursor::Rotation::k180:
      return "180";
    case drm::cursor::Rotation::k270:
      return "270";
  }
  return "?";
}

struct CrtcInfo {
  std::uint32_t crtc_id{0};
  std::uint32_t mode_w{0};
  std::uint32_t mode_h{0};
};

// Every connected connector with an active mode gets a CRTC entry.
// Multi-head systems produce multiple; on single-head we still bind
// the shared_ptr to the one we found, which exercises the API.
std::vector<CrtcInfo> discover_active_crtcs(const int fd) {
  std::vector<CrtcInfo> out;
  const auto res = drm::get_resources(fd);
  if (!res) {
    return out;
  }
  for (int i = 0; i < res->count_connectors; ++i) {
    const auto conn = drm::get_connector(fd, res->connectors[i]);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0 ||
        conn->encoder_id == 0) {
      continue;
    }
    const auto enc = drm::get_encoder(fd, conn->encoder_id);
    if (!enc || enc->crtc_id == 0) {
      continue;
    }
    const auto crtc = drm::get_crtc(fd, enc->crtc_id);
    if (!crtc || crtc->mode_valid == 0) {
      continue;
    }
    out.push_back({enc->crtc_id, crtc->mode.hdisplay, crtc->mode.vdisplay});
  }
  return out;
}

// Parse a non-negative integer with overflow detection via from_chars
// — unlike strtol, it doesn't need errno and happily takes a
// string_view, so the endptr/pointee-const issue that trips
// misc-const-correctness in the strtol variant doesn't apply here.
bool parse_uint(const char* s, int max_val, int& out) {
  if (s == nullptr || *s == '\0') {
    return false;
  }
  const std::string_view sv(s);
  int v = 0;
  const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
  if (ec != std::errc{} || ptr != sv.data() + sv.size() || v < 0 || v > max_val) {
    return false;
  }
  out = v;
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  // ---------------------------------------------------------------------------
  // CLI parse. Strip our own flags before handing argv to select_device.
  // ---------------------------------------------------------------------------
  const auto *cli_theme = "Adwaita";
  const auto *cli_cursor = "default";
  int cli_size = 0;
  int cli_period = 2000;
  bool cli_no_rotate = false;

  auto strip = [&](const int i, const int n) {
    for (int j = i; j + n < argc; ++j) {
      argv[j] = argv[j + n];
    }
    argc -= n;
  };

  for (int i = 1; i < argc;) {
    if (std::strcmp(argv[i], "--no-rotate") == 0) {
      cli_no_rotate = true;
      strip(i, 1);
      continue;
    }
    const bool is_theme = std::strcmp(argv[i], "--theme") == 0;
    const bool is_cursor = std::strcmp(argv[i], "--cursor") == 0;
    const bool is_size = std::strcmp(argv[i], "--size") == 0;
    const bool is_period = std::strcmp(argv[i], "--period") == 0;
    if (is_theme || is_cursor || is_size || is_period) {
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
          drm::println(stderr, "--size: invalid '{}'", argv[i + 1]);
          return EXIT_FAILURE;
        }
      } else if (!parse_uint(argv[i + 1], 600000, cli_period)) {
        drm::println(stderr, "--period: invalid '{}'", argv[i + 1]);
        return EXIT_FAILURE;
      }
      strip(i, 2);
      continue;
    }
    ++i;
  }

  // ---------------------------------------------------------------------------
  // Device + CRTC discovery. Multi-CRTC enumeration is the example's
  // whole point, so the connector pickup stays bespoke; only the open
  // half is shared via open_device.
  // ---------------------------------------------------------------------------
  auto ctx = drm::examples::open_device(argc, argv);
  if (!ctx) {
    return EXIT_FAILURE;
  }
  auto& dev = ctx->device;

  const auto crtcs = discover_active_crtcs(dev.fd());
  if (crtcs.empty()) {
    drm::println(stderr, "No active CRTCs found");
    return EXIT_FAILURE;
  }

  // ---------------------------------------------------------------------------
  // Item 1 — theme resolve cache. Two back-to-back resolves; time both.
  // ---------------------------------------------------------------------------
  auto theme = drm::cursor::Theme::discover();
  if (!theme) {
    drm::println(stderr, "Theme discover failed: {}", theme.error().message());
    return EXIT_FAILURE;
  }

  const auto start_first = std::chrono::steady_clock::now();
  auto first = theme->resolve(cli_cursor, cli_theme);
  const auto start_second = std::chrono::steady_clock::now();
  auto second = theme->resolve(cli_cursor, cli_theme);
  const auto end_second = std::chrono::steady_clock::now();
  if (!first || !second) {
    drm::println(stderr, "Theme resolve failed: {}", first.error().message());
    return EXIT_FAILURE;
  }
  const auto miss_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(start_second - start_first).count();
  const auto hit_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end_second - start_second).count();
  drm::println(
      "[item 1] resolve('{}', '{}') — first {} ns (miss), second {} ns (hit), resolved theme={}",
      cli_cursor, cli_theme, miss_ns, hit_ns, first->theme_name);

  // ---------------------------------------------------------------------------
  // Item 3 — shared Cursor. One load, shared_ptr handed to every renderer.
  // ---------------------------------------------------------------------------
  auto loaded = drm::cursor::Cursor::load(*first, static_cast<std::uint32_t>(cli_size));
  if (!loaded) {
    drm::println(stderr, "Cursor::load failed: {}", loaded.error().message());
    return EXIT_FAILURE;
  }
  const auto shared_cursor = std::make_shared<drm::cursor::Cursor>(std::move(*loaded));
  drm::println(
      "[item 3] shared Cursor loaded ({} frame(s), animated={}) — share count before bind: {}",
      shared_cursor->frame_count(), shared_cursor->animated(), shared_cursor.use_count());

  // ---------------------------------------------------------------------------
  // Per-CRTC Renderers. Each bind share_ptrs once so the final use_count
  // reports how many heads hold a reference.
  // ---------------------------------------------------------------------------
  std::vector<drm::cursor::Renderer> renderers;
  renderers.reserve(crtcs.size());
  for (const auto& [crtc_id, mode_w, mode_h] : crtcs) {
    drm::cursor::RendererConfig cfg;
    cfg.crtc_id = crtc_id;
    auto r = drm::cursor::Renderer::create(dev, cfg);
    if (!r) {
      drm::println(stderr, "Renderer::create(crtc={}) failed: {}", crtc_id,
                   r.error().message());
      return EXIT_FAILURE;
    }
    auto& rend = renderers.emplace_back(std::move(*r));

    // Items 2 + 5 per-renderer introspection.
    drm::println("[crtc {}] {}x{}  plane_id={}  path={}  hw_rotation={}  hotspot_props={}",
                 crtc_id, mode_w, mode_h, rend.plane_id(), path_name(rend.path()),
                 rend.has_hardware_rotation() ? "yes" : "no",
                 rend.has_hotspot_properties() ? "yes" : "no");

    if (auto set = rend.set_cursor(shared_cursor); !set) {
      drm::println(stderr, "set_cursor(crtc={}) failed: {}", crtc_id, set.error().message());
      return EXIT_FAILURE;
    }
    if (auto mv =
            rend.move_to(static_cast<int>(mode_w) / 2, static_cast<int>(mode_h) / 2);
        !mv) {
      drm::println(stderr, "move_to(crtc={}) failed: {}", crtc_id, mv.error().message());
      return EXIT_FAILURE;
    }
  }
  drm::println("[item 3] shared Cursor use_count after binding {} renderer(s): {}",
               renderers.size(), shared_cursor.use_count());

  if (cli_no_rotate) {
    drm::println("[item 4] rotation cycle skipped (--no-rotate). Initial state is rotation=0.");
    return EXIT_SUCCESS;
  }

  // ---------------------------------------------------------------------------
  // Items 4 + 5 — rotation cycle. set_rotation() is dispatched across all
  // bound renderers; each reports which path (hw vs sw) handled it via
  // has_hardware_rotation().
  //
  // Exit handling: SIGINT/SIGTERM cover the case where stdin is still a
  // line-discipline TTY, but when libseat takes the seat the kernel puts
  // the TTY into KD_GRAPHICS and stops translating ^C — so we also open
  // a libinput keyboard and quit on Esc/q. The same input source carries
  // Ctrl+Alt+F<n> for VT switching (routed through VtChordTracker).
  // ---------------------------------------------------------------------------
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  drm::input::InputDeviceOpener libinput_opener;
  if (ctx->seat) {
    libinput_opener = ctx->seat->input_opener();
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
      if (vt_chord.observe(*ke, ctx->seat ? &*ctx->seat : nullptr)) {
        return;
      }
      if (vt_chord.is_quit_key(*ke)) {
        g_quit = 1;
      }
    });
  } else {
    drm::println(stderr,
                 "input::Seat::open: {} — Esc/q exit unavailable (need 'input' group or a seat "
                 "backend); Ctrl-C only works on a non-graphics TTY",
                 input_seat_res.error().message());
  }

  // Seat pause/resume. Each Renderer holds an fd snapshot, so on
  // resume every renderer needs on_session_resumed(new_fd); the Device
  // wrapper itself also gets swapped so client caps (UNIVERSAL_PLANES,
  // ATOMIC) can be re-enabled on the fresh fd before the next atomic
  // commit. Work happens in the main loop, not the libseat callback,
  // so a transient drmIsMaster lag at handover gets a retry instead of
  // a hard fail.
  bool session_paused = false;
  int pending_resume_fd = -1;
  if (ctx->seat) {
    ctx->seat->set_pause_callback([&]() {
      session_paused = true;
      for (auto& rend : renderers) {
        rend.on_session_paused();
      }
      if (input_seat) {
        (void)input_seat->suspend();
      }
    });
    ctx->seat->set_resume_callback([&](std::string_view /*path*/, int new_fd) {
      pending_resume_fd = new_fd;
      session_paused = false;
      if (input_seat) {
        (void)input_seat->resume();
      }
    });
  }

  constexpr std::array<drm::cursor::Rotation, 4> k_cycle = {
      drm::cursor::Rotation::k0,
      drm::cursor::Rotation::k90,
      drm::cursor::Rotation::k180,
      drm::cursor::Rotation::k270,
  };

  const auto period = std::chrono::milliseconds(cli_period);
  drm::println("[item 4] cycling rotation every {} ms ({} to exit)...", cli_period,
               input_seat ? "Esc/q or Ctrl-C" : "Ctrl-C");

  std::array<pollfd, 2> pfds{};
  pfds[0].fd = input_seat ? input_seat->fd() : -1;
  pfds[0].events = POLLIN;
  pfds[1].fd = ctx->seat ? ctx->seat->poll_fd() : -1;
  pfds[1].events = POLLIN;

  std::size_t idx = 0;
  auto next_change = std::chrono::steady_clock::now();
  while (g_quit == 0) {
    if (pending_resume_fd != -1) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      // dev is a reference into ctx->device; move-assigning swaps the
      // underlying Device so renderer fd-snapshots refresh below.
      ctx->device = drm::Device::from_fd(new_fd);
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed: {}", r.error().message());
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed: {}", r.error().message());
        break;
      }
      for (std::size_t i = 0; i < renderers.size(); ++i) {
        if (auto r = renderers.at(i).on_session_resumed(new_fd); !r) {
          drm::println(stderr, "Renderer resume (crtc={}) failed: {}", crtcs.at(i).crtc_id,
                       r.error().message());
        }
      }
      // Don't blast through the rotation backlog accumulated while paused.
      next_change = std::chrono::steady_clock::now() + period;
    }

    if (!session_paused) {
      if (const auto now = std::chrono::steady_clock::now(); now >= next_change) {
        const auto rot = k_cycle.at(idx);
        idx = (idx + 1) % k_cycle.size();
        for (std::size_t i = 0; i < renderers.size(); ++i) {
          auto& rend = renderers.at(i);
          if (auto sr = rend.set_rotation(rot); !sr) {
            drm::println(stderr, "set_rotation(crtc={}, rot={}): {}", crtcs.at(i).crtc_id,
                         rotation_name(rot), sr.error().message());
          } else {
            drm::println("  crtc={} rotation={} via {}", crtcs.at(i).crtc_id, rotation_name(rot),
                         rend.has_hardware_rotation() ? "hardware" : "software blit");
          }
        }
        next_change = now + period;
      }

      // Advance animation for animated cursors (wait, progress, etc.).
      for (auto& rend : renderers) {
        (void)rend.tick();
      }
    }

    // Paused: block until the seat fd wakes us with a resume event.
    // Active: wake on input or at the next rotation step, whichever is
    // sooner; 16 ms cap keeps animated cursors at refresh-ish cadence.
    int timeout_ms = -1;
    if (!session_paused) {
      const auto until_next =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              next_change - std::chrono::steady_clock::now())
              .count();
      timeout_ms = static_cast<int>(std::clamp<long long>(until_next, 0, 16));
    }
    if (const int rc = poll(pfds.data(), pfds.size(), timeout_ms); rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if (input_seat && (pfds[0].revents & POLLIN) != 0) {
      if (auto r = input_seat->dispatch(); !r) {
        drm::println(stderr, "input dispatch failed: {}", r.error().message());
      }
    }
    if (ctx->seat && (pfds[1].revents & POLLIN) != 0) {
      ctx->seat->dispatch();
    }
  }

  drm::println("cursor_rotate: exiting");
  return EXIT_SUCCESS;
}