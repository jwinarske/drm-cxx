// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cursor_rotate — hardware validation harness for drm::cursor V2.
//
// Usage: cursor_rotate [--theme NAME] [--cursor NAME] [--size N]
//                     [--period MS] [--no-rotate] [/dev/dri/cardN]
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
#include "core/device.hpp"
#include "core/resources.hpp"
#include "cursor/cursor.hpp"
#include "cursor/renderer.hpp"
#include "cursor/theme.hpp"
#include "drm-cxx/detail/format.hpp"

#include <xf86drmMode.h>

#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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
std::vector<CrtcInfo> discover_active_crtcs(int fd) {
  std::vector<CrtcInfo> out;
  const auto res = drm::get_resources(fd);
  if (!res) {
    return out;
  }
  for (int i = 0; i < res->count_connectors; ++i) {
    auto conn = drm::get_connector(fd, res->connectors[i]);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0 ||
        conn->encoder_id == 0) {
      continue;
    }
    auto enc = drm::get_encoder(fd, conn->encoder_id);
    if (!enc || enc->crtc_id == 0) {
      continue;
    }
    auto crtc = drm::get_crtc(fd, enc->crtc_id);
    if (!crtc || crtc->mode_valid == 0) {
      continue;
    }
    out.push_back({enc->crtc_id, crtc->mode.hdisplay, crtc->mode.vdisplay});
  }
  return out;
}

// Parse a non-negative integer with overflow detection via from_chars
// — unlike strtol, it doesn't need errno and happily takes a
// string_view so the endptr/pointee-const issue that trips
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
  const char* cli_theme = "Adwaita";
  const char* cli_cursor = "default";
  int cli_size = 0;
  int cli_period = 2000;
  bool cli_no_rotate = false;

  auto strip = [&](int i, int n) {
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
  for (const auto& crtc : crtcs) {
    drm::cursor::RendererConfig cfg;
    cfg.crtc_id = crtc.crtc_id;
    auto r = drm::cursor::Renderer::create(dev, cfg);
    if (!r) {
      drm::println(stderr, "Renderer::create(crtc={}) failed: {}", crtc.crtc_id,
                   r.error().message());
      return EXIT_FAILURE;
    }
    auto& rend = renderers.emplace_back(std::move(*r));

    // Items 2 + 5 per-renderer introspection.
    drm::println("[crtc {}] {}x{}  plane_id={}  path={}  hw_rotation={}  hotspot_props={}",
                 crtc.crtc_id, crtc.mode_w, crtc.mode_h, rend.plane_id(), path_name(rend.path()),
                 rend.has_hardware_rotation() ? "yes" : "no",
                 rend.has_hotspot_properties() ? "yes" : "no");

    if (auto set = rend.set_cursor(shared_cursor); !set) {
      drm::println(stderr, "set_cursor(crtc={}) failed: {}", crtc.crtc_id, set.error().message());
      return EXIT_FAILURE;
    }
    if (auto mv =
            rend.move_to(static_cast<int>(crtc.mode_w) / 2, static_cast<int>(crtc.mode_h) / 2);
        !mv) {
      drm::println(stderr, "move_to(crtc={}) failed: {}", crtc.crtc_id, mv.error().message());
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
  // ---------------------------------------------------------------------------
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  constexpr std::array<drm::cursor::Rotation, 4> k_cycle = {
      drm::cursor::Rotation::k0,
      drm::cursor::Rotation::k90,
      drm::cursor::Rotation::k180,
      drm::cursor::Rotation::k270,
  };

  const auto period = std::chrono::milliseconds(cli_period);
  drm::println("[item 4] cycling rotation every {} ms (Ctrl-C to exit)...", cli_period);

  std::size_t idx = 0;
  auto next_change = std::chrono::steady_clock::now();
  while (g_quit == 0) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_change) {
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

    // Advance animation for animated cursors (wait, progress, etc.)
    // and sleep a short tick so Ctrl-C responds promptly.
    for (auto& rend : renderers) {
      (void)rend.tick();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  drm::println("cursor_rotate: exiting on signal");
  return EXIT_SUCCESS;
}