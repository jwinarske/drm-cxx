// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// mdi_demo — multi-document desktop on top of drm::csd.
//
// Each "document" is a glass-themed decoration painted by
// drm::csd::Renderer into a drm::csd::Surface and arrived on its own
// reserved DRM overlay plane via drm::csd::PlanePresenter (Tier 0).
// The Shell type (shell.hpp) holds the document list, focus stack, and
// hit testing — main.cpp wires it to libinput, the bg LayerScene, and
// the per-frame atomic commit.
//
// Controls (TTY only — chord requires a libseat session, see below):
//
//   Mouse left click on title    Focus + drag the doc by its title bar.
//   Mouse left click on close    Close the focused doc.
//   Ctrl+Tab                     Send focused doc to the back of the stack.
//   Ctrl+N                       Spawn a new doc (capped at the plane budget).
//   Ctrl+W                       Close the focused doc (alternative to clicking close).
//   Ctrl+S                       Snapshot the CRTC to --dump (when set).
//   Esc / Q / Ctrl+C             Quit.
//   Ctrl+Alt+F<n>                Switch VT (libseat sessions only).
//
// Usage:
//   mdi_demo [--docs N] [--theme {default|lite|minimal|PATH}]
//            [--dump PATH.png] [--presenter {plane|composite|fb}]
//            [/dev/dri/cardN]
//
// Preconditions match every other csd example: run from a TTY (Ctrl-
// Alt-F3) or a libseat session. Build needs DRM_CXX_HAS_BLEND2D=1.

#include "../common/cursor_size.hpp"
#include "../common/open_output.hpp"
#include "../common/vt_switch.hpp"
#include "csd/overlay_reservation.hpp"
#include "csd/presenter.hpp"
#include "csd/presenter_plane.hpp"
#include "csd/theme.hpp"
#include "cursor/cursor.hpp"
#include "cursor/renderer.hpp"
#include "cursor/theme.hpp"
#include "gbm/device.hpp"
#include "input/seat.hpp"
#include "planes/plane_registry.hpp"
#include "scene/dumb_buffer_source.hpp"
#include "scene/layer_desc.hpp"
#include "scene/layer_scene.hpp"
#include "shell.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/capture/png.hpp>
#include <drm-cxx/capture/snapshot.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/atomic.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
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

enum class PresenterMode : std::uint8_t { Plane, Composite, Fb };

struct Args {
  std::uint32_t initial_docs{2};
  std::string theme_name{"default"};
  std::string dump_path;
  PresenterMode presenter{PresenterMode::Plane};
};

// Resolve a --theme value to a const Theme*. Built-in names map to the
// stable references; paths get parsed once into `out_storage` and a
// pointer to it is returned. Returns nullptr only on parse failure of
// a TOML path; built-in names are total.
const drm::csd::Theme* resolve_theme(std::string_view name, drm::csd::Theme& out_storage) {
  if (name == "default") {
    return &drm::csd::glass_default_theme();
  }
  if (name == "lite") {
    return &drm::csd::glass_lite_theme();
  }
  if (name == "minimal") {
    return &drm::csd::glass_minimal_theme();
  }
  // Anything else is a path. Layer onto glass_default so missing keys
  // inherit the desktop tier defaults rather than zero-initialized
  // garbage (load_theme_file's contract).
  auto loaded = drm::csd::load_theme_file(name, drm::csd::glass_default_theme());
  if (!loaded) {
    drm::println(stderr, "mdi_demo: load_theme_file({}): {}", name, loaded.error().message());
    return nullptr;
  }
  out_storage = std::move(*loaded);
  return &out_storage;
}

// Locate the ordinal index of `crtc_id` within drmModeRes::crtcs[].
// PlaneRegistry::for_crtc and OverlayReservation::reserve both want
// the index, but most other drm-cxx APIs work with the KMS object id;
// we need both. Mirrors the helper in csd_smoke.
std::optional<std::uint32_t> crtc_index_for(int fd, std::uint32_t crtc_id) {
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return std::nullopt;
  }
  std::optional<std::uint32_t> out;
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtc_id) {
      out = static_cast<std::uint32_t>(i);
      break;
    }
  }
  drmModeFreeResources(res);
  return out;
}

// Highest zpos any PRIMARY plane on this CRTC reaches. Used as the
// floor for overlay stacking — base_zpos = primary_zpos_max + 1
// lands the decorations directly above the bg. Mirrors csd_smoke's
// helper of the same name.
std::uint64_t primary_zpos_max(const drm::planes::PlaneRegistry& registry,
                               std::uint32_t crtc_index) {
  std::uint64_t out = 0;
  for (const auto* cap : registry.for_crtc(crtc_index)) {
    if (cap == nullptr || cap->type != drm::planes::DRMPlaneType::PRIMARY) {
      continue;
    }
    if (cap->zpos_max.has_value()) {
      out = std::max(out, *cap->zpos_max);
    }
  }
  return out;
}

// Try to reserve `desired` overlays for `crtc_index`, falling back to
// progressively smaller counts when the registry doesn't have enough
// candidates. Returns the actual count reserved (>= 1) on success;
// 0 means no compatible overlay was available at all and the demo
// can't run on this CRTC.
std::vector<std::uint32_t> reserve_with_fallback(drm::csd::OverlayReservation& reservation,
                                                 std::uint32_t crtc_index, std::size_t desired) {
  for (std::size_t count = desired; count >= 1; --count) {
    auto r = reservation.reserve(crtc_index, DRM_FORMAT_ARGB8888, count);
    if (r) {
      return std::move(*r);
    }
  }
  return {};
}

// Paint a vertical blue→teal gradient into a CPU-linear XRGB8888 bg.
// Different visual signature from csd_smoke's horizontal black/white
// so screenshots of the two demos don't get confused at a glance.
void paint_bg_gradient(drm::span<std::uint8_t> pixels, std::uint32_t stride, std::uint32_t w,
                       std::uint32_t h) {
  for (std::uint32_t y = 0; y < h; ++y) {
    auto* row = pixels.data() + (static_cast<std::size_t>(y) * stride);
    const auto t = (h <= 1) ? 0U : (y * 255U) / (h - 1U);
    const std::uint8_t blue_b = 80;
    const std::uint8_t blue_g = 30;
    const std::uint8_t blue_r = 20;
    const std::uint8_t teal_b = 110;
    const std::uint8_t teal_g = 90;
    const std::uint8_t teal_r = 40;
    const auto lerp = [&](std::uint8_t a, std::uint8_t b) -> std::uint8_t {
      return static_cast<std::uint8_t>(((255U - t) * a + t * b) / 255U);
    };
    for (std::uint32_t x = 0; x < w; ++x) {
      row[(x * 4U) + 0U] = lerp(blue_b, teal_b);
      row[(x * 4U) + 1U] = lerp(blue_g, teal_g);
      row[(x * 4U) + 2U] = lerp(blue_r, teal_r);
      row[(x * 4U) + 3U] = 0;
    }
  }
}

// Strip our flags from argv so select_device only sees the optional
// device path. Same idiom as csd_smoke / mouse_cursor.
Args parse_args(int& argc, char* argv[]) {
  Args a;
  auto strip = [&](int i, int n) {
    for (int j = i; j + n < argc; ++j) {
      argv[j] = argv[j + n];
    }
    argc -= n;
  };
  auto match = [&](std::string_view arg, std::string_view name,
                   int i) -> std::pair<std::optional<std::string_view>, int> {
    if (arg == name && i + 1 < argc) {
      return {std::string_view{argv[i + 1]}, 2};
    }
    if (arg.size() > name.size() + 1 && arg.substr(0, name.size()) == name &&
        arg[name.size()] == '=') {
      return {arg.substr(name.size() + 1), 1};
    }
    return {std::nullopt, 0};
  };
  for (int i = 1; i < argc;) {
    const std::string_view arg = argv[i];
    if (auto [v, n] = match(arg, "--docs", i); v) {
      try {
        a.initial_docs = static_cast<std::uint32_t>(std::stoul(std::string(*v)));
      } catch (...) {
        drm::println(stderr, "mdi_demo: --docs: invalid value '{}'", *v);
      }
      strip(i, n);
    } else if (auto [v2, n2] = match(arg, "--theme", i); v2) {
      a.theme_name = std::string(*v2);
      strip(i, n2);
    } else if (auto [v3, n3] = match(arg, "--dump", i); v3) {
      a.dump_path = std::string(*v3);
      strip(i, n3);
    } else if (auto [v4, n4] = match(arg, "--presenter", i); v4) {
      if (*v4 == "plane") {
        a.presenter = PresenterMode::Plane;
      } else if (*v4 == "composite") {
        a.presenter = PresenterMode::Composite;
      } else if (*v4 == "fb") {
        a.presenter = PresenterMode::Fb;
      } else {
        drm::println(stderr, "mdi_demo: --presenter: unknown value '{}', defaulting to plane", *v4);
      }
      strip(i, n4);
    } else {
      ++i;
    }
  }
  return a;
}

}  // namespace

int main(int argc, char* argv[]) {
  const Args args = parse_args(argc, argv);

  if (args.presenter != PresenterMode::Plane) {
    drm::println(stderr,
                 "mdi_demo: --presenter={} is not implemented in v1; only the Tier 0 Plane "
                 "presenter has landed. Re-run with --presenter=plane (or omit the flag).",
                 args.presenter == PresenterMode::Composite ? "composite" : "fb");
    return EXIT_FAILURE;
  }

  // ── Theme resolution ─────────────────────────────────────────────
  drm::csd::Theme theme_from_file{};  // backing storage for path-loaded themes
  const drm::csd::Theme* theme = resolve_theme(args.theme_name, theme_from_file);
  if (theme == nullptr) {
    return EXIT_FAILURE;
  }
  drm::println("mdi_demo: theme = {}", theme->name);

  // ── Device + output ──────────────────────────────────────────────
  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  auto& seat = output->seat;
  const drmModeModeInfo mode = output->mode;
  const std::uint32_t fb_w = mode.hdisplay;
  const std::uint32_t fb_h = mode.vdisplay;
  drm::println("mdi_demo: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h, mode.vrefresh,
               output->connector_id, output->crtc_id);

  // ── GBM (optional) ───────────────────────────────────────────────
  std::optional<drm::gbm::GbmDevice> gbm;
  if (auto g = drm::gbm::GbmDevice::create(dev.fd()); g) {
    gbm.emplace(std::move(*g));
  } else {
    drm::println("mdi_demo: GbmDevice::create failed ({}); decorations use the dumb path",
                 g.error().message());
  }

  // ── PlaneRegistry + OverlayReservation ───────────────────────────
  auto registry_res = drm::planes::PlaneRegistry::enumerate(dev);
  if (!registry_res) {
    drm::println(stderr, "mdi_demo: PlaneRegistry::enumerate: {}", registry_res.error().message());
    return EXIT_FAILURE;
  }
  const auto registry = std::move(*registry_res);

  const auto crtc_idx = crtc_index_for(dev.fd(), output->crtc_id);
  if (!crtc_idx) {
    drm::println(stderr, "mdi_demo: crtc_index_for({}): not found", output->crtc_id);
    return EXIT_FAILURE;
  }

  auto reservation_res = drm::csd::OverlayReservation::create(registry);
  if (!reservation_res) {
    drm::println(stderr, "mdi_demo: OverlayReservation::create: {}",
                 reservation_res.error().message());
    return EXIT_FAILURE;
  }
  auto reservation = std::move(*reservation_res);

  // Try to reserve `--docs` overlays for decorations; fall back to
  // smaller counts so the demo runs on plane-budget-limited hardware
  // without forcing the user to guess the limit. The budget defines
  // the upper bound on documents — Ctrl+N stops spawning at that
  // ceiling. A budget of 0 means no compatible overlay exists.
  const auto reserved = reserve_with_fallback(reservation, *crtc_idx, args.initial_docs);
  if (reserved.empty()) {
    drm::println(stderr,
                 "mdi_demo: no ARGB8888 overlay plane available on CRTC {} — needs at least 1 "
                 "for the decoration. Try a different card or run on hardware with overlay "
                 "support.",
                 output->crtc_id);
    return EXIT_FAILURE;
  }
  if (reserved.size() < args.initial_docs) {
    drm::println("mdi_demo: --docs={} requested, plane budget {} available; capping",
                 args.initial_docs, reserved.size());
  }
  drm::println("mdi_demo: reserved {} overlay plane{} for decorations", reserved.size(),
               reserved.size() == 1 ? "" : "s");

  // ── PlanePresenter ───────────────────────────────────────────────
  const std::uint64_t base_zpos = primary_zpos_max(registry, *crtc_idx) + 1U;
  auto presenter_res = drm::csd::PlanePresenter::create(
      dev, registry, output->crtc_id,
      drm::span<const std::uint32_t>(reserved.data(), reserved.size()), base_zpos);
  if (!presenter_res) {
    drm::println(stderr, "mdi_demo: PlanePresenter::create: {}", presenter_res.error().message());
    return EXIT_FAILURE;
  }
  const auto presenter = std::move(*presenter_res);
  drm::println("mdi_demo: PlanePresenter armed (base_zpos={})", base_zpos);

  // ── Background LayerScene ────────────────────────────────────────
  // Bg modeset commits once at startup. The primary plane's atomic
  // state survives every per-frame overlay-only commit afterward, so
  // the bg keeps scanning out without explicit re-arming.
  auto bg_src_res = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src_res) {
    drm::println(stderr, "mdi_demo: DumbBufferSource::create (bg): {}",
                 bg_src_res.error().message());
    return EXIT_FAILURE;
  }
  auto bg_src = std::move(*bg_src_res);
  {
    auto map = bg_src->map(drm::MapAccess::Write);
    if (!map) {
      drm::println(stderr, "mdi_demo: bg.map: {}", map.error().message());
      return EXIT_FAILURE;
    }
    paint_bg_gradient(map->pixels(), map->stride(), fb_w, fb_h);
  }

  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = output->crtc_id;
  scene_cfg.connector_id = output->connector_id;
  scene_cfg.mode = mode;
  auto scene_res = drm::scene::LayerScene::create(dev, scene_cfg);
  if (!scene_res) {
    drm::println(stderr, "mdi_demo: LayerScene::create: {}", scene_res.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_res);

  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(bg_src);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  if (auto r = scene->add_layer(std::move(bg_desc)); !r) {
    drm::println(stderr, "mdi_demo: scene.add_layer (bg): {}", r.error().message());
    return EXIT_FAILURE;
  }

  drm::PageFlip page_flip(dev);
  bool flipped = false;
  page_flip.set_handler(
      [&](std::uint32_t /*crtc*/, std::uint64_t /*seq*/, std::uint64_t /*ts*/) { flipped = true; });

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "mdi_demo: scene.commit (bg modeset): {}", r.error().message());
    return EXIT_FAILURE;
  }
  while (!flipped) {
    if (auto r = page_flip.dispatch(-1); !r) {
      drm::println(stderr, "mdi_demo: page_flip.dispatch (modeset): {}", r.error().message());
      return EXIT_FAILURE;
    }
  }

  // ── Shell + initial documents ────────────────────────────────────
  mdi_demo::Shell shell(dev, gbm ? &*gbm : nullptr, *theme, reserved.size());
  for (std::size_t i = 0; i < reserved.size(); ++i) {
    if (!shell.spawn_document(fb_w, fb_h)) {
      break;
    }
  }
  drm::println("mdi_demo: spawned {} document{}", shell.document_count(),
               shell.document_count() == 1 ? "" : "s");

  // ── Input ────────────────────────────────────────────────────────
  drm::input::InputDeviceOpener input_opener;
  if (seat) {
    input_opener = seat->input_opener();
  }
  auto input_seat_res = drm::input::Seat::open({}, std::move(input_opener));
  if (!input_seat_res) {
    drm::println(stderr, "mdi_demo: input::Seat::open: {} (need root or input group)",
                 input_seat_res.error().message());
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_res;

  // Pointer state. Track the last-known position so PointerMotionEvent
  // deltas can be turned into absolute coords for the shell.
  double pointer_x = static_cast<double>(fb_w) / 2.0;
  double pointer_y = static_cast<double>(fb_h) / 2.0;
  bool ctrl_left = false;
  bool ctrl_right = false;
  const auto ctrl_held = [&]() { return ctrl_left || ctrl_right; };

  // Frame dirty flag — set by any input that should trigger a present.
  // The main loop polls this to decide whether to issue a new commit.
  bool frame_dirty = true;
  // Snapshot request — Ctrl+S sets this; the main loop services it
  // off the input thread.
  bool snapshot_requested = false;

  // ── Cursor sprite (best-effort) ─────────────────────────────────
  // Loads the system XCursor "default" + "grabbing" shapes and tracks
  // the pointer with drm::cursor::Renderer. The renderer prefers a
  // dedicated DRM CURSOR plane (free, doesn't compete with the
  // overlay budget); it falls back to OVERLAY/legacy when none is
  // exposed. Any failure (no theme installed, no cursor-capable
  // plane on this CRTC, missing shape file) is non-fatal — we log
  // and continue with no on-screen sprite. The drag still works in
  // either case because position lives in the software pointer_x/y
  // pair the input handler maintains.
  std::optional<drm::cursor::Renderer> cursor_renderer;
  std::shared_ptr<const drm::cursor::Cursor> cursor_default;
  std::shared_ptr<const drm::cursor::Cursor> cursor_grabbing;
  bool cursor_dirty = true;
  bool was_dragging = false;
  if (auto theme = drm::cursor::Theme::discover(); !theme) {
    drm::println("mdi_demo: no XCursor theme on the system search path; running without sprite");
  } else {
    std::uint64_t cap_w = 0;
    drmGetCap(dev.fd(), DRM_CAP_CURSOR_WIDTH, &cap_w);
    const auto sizing = drm::examples::cursor_sizing_for_output(dev.fd(), output->connector_id,
                                                                output->mode, 16U, cap_w);
    auto load_shape = [&](std::string_view name) -> std::shared_ptr<const drm::cursor::Cursor> {
      auto c = drm::cursor::Cursor::load(*theme, name, std::string_view{}, sizing.sprite);
      if (!c) {
        return nullptr;
      }
      return std::make_shared<const drm::cursor::Cursor>(std::move(*c));
    };
    cursor_default = load_shape("default");
    if (!cursor_default) {
      drm::println("mdi_demo: cursor 'default' missing in theme; running without sprite");
    } else {
      cursor_grabbing = load_shape("grabbing");
      if (!cursor_grabbing) {
        // 'grabbing' is best-effort — fall back to the arrow so the
        // drag still works, just without a visual cue.
        drm::println("mdi_demo: cursor 'grabbing' missing in theme; using arrow during drag");
        cursor_grabbing = cursor_default;
      }
      drm::cursor::RendererConfig cur_cfg;
      cur_cfg.crtc_id = output->crtc_id;
      cur_cfg.preferred_size = sizing.buffer;
      auto r = drm::cursor::Renderer::create(dev, cur_cfg);
      if (!r) {
        drm::println("mdi_demo: cursor::Renderer::create: {}; running without sprite",
                     r.error().message());
      } else if (auto sr = r->set_cursor(cursor_default); !sr) {
        drm::println("mdi_demo: cursor set_cursor: {}; running without sprite",
                     sr.error().message());
      } else {
        cursor_renderer.emplace(std::move(*r));
      }
    }
  }

  drm::examples::VtChordTracker vt_chord;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (vt_chord.observe(*ke, seat ? &*seat : nullptr)) {
        if (ke->key == KEY_LEFTCTRL) {
          ctrl_left = ke->pressed;
        } else if (ke->key == KEY_RIGHTCTRL) {
          ctrl_right = ke->pressed;
        }
        return;
      }
      if (vt_chord.is_quit_key(*ke)) {
        shell.request_quit();
        return;
      }
      if (!ke->pressed) {
        return;
      }
      if (ctrl_held() && ke->key == KEY_TAB) {
        shell.cycle_focus();
        frame_dirty = true;
        return;
      }
      if (ctrl_held() && ke->key == KEY_N) {
        if (shell.spawn_document(fb_w, fb_h)) {
          frame_dirty = true;
        }
        return;
      }
      if (ctrl_held() && ke->key == KEY_W) {
        shell.close_focused();
        frame_dirty = true;
        return;
      }
      if (ctrl_held() && ke->key == KEY_S) {
        snapshot_requested = true;
        return;
      }
    }
    if (const auto* pe = std::get_if<drm::input::PointerEvent>(&event)) {
      if (const auto* m = std::get_if<drm::input::PointerMotionEvent>(pe)) {
        pointer_x = std::clamp(pointer_x + m->dx, 0.0, static_cast<double>(fb_w - 1U));
        pointer_y = std::clamp(pointer_y + m->dy, 0.0, static_cast<double>(fb_h - 1U));
        cursor_dirty = true;
        if (shell.on_pointer_motion(static_cast<std::int32_t>(pointer_x),
                                    static_cast<std::int32_t>(pointer_y))) {
          frame_dirty = true;
        }
      } else if (const auto* b = std::get_if<drm::input::PointerButtonEvent>(pe)) {
        if (b->button != BTN_LEFT) {
          return;
        }
        if (b->pressed) {
          if (shell.on_pointer_press(static_cast<std::int32_t>(pointer_x),
                                     static_cast<std::int32_t>(pointer_y))) {
            frame_dirty = true;
          }
        } else {
          shell.on_pointer_release();
        }
      }
    }
  });

  // ── Session pause/resume hooks ──────────────────────────────────
  // Keep the example honest about VT-switch behavior: forward the
  // libseat callbacks to the input seat so libinput's grabbed input
  // fds get released, and bail out on resume so the user re-runs the
  // demo on the new fd. Full mid-flight resume (rebuild GBM,
  // PlaneRegistry, PlanePresenter, every csd::Surface against the
  // fresh fd) is in the same shape as cursor::Renderer's resume but
  // hasn't been wired yet for csd — out of scope for v1.
  if (seat) {
    seat->set_pause_callback([&]() { (void)input_seat.suspend(); });
    seat->set_resume_callback([&](std::string_view /*path*/, int /*new_fd*/) {
      drm::println("mdi_demo: session resumed; csd resume isn't wired yet — exiting");
      shell.request_quit();
      (void)input_seat.resume();
    });
  }

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  drm::println(
      "mdi_demo: ready. Drag title bars to move; click close to remove; "
      "Ctrl+Tab cycles focus; Esc to quit.");

  // ── Main loop ────────────────────────────────────────────────────
  pollfd pfds[2]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = seat ? seat->poll_fd() : -1;
  pfds[1].events = POLLIN;

  auto last_tick = std::chrono::steady_clock::now();
  while (g_quit == 0 && !shell.quit_requested()) {
    // Wake roughly every ~16 ms even when idle: lets us spot quit
    // signals and snapshot requests promptly without spinning, and
    // gives the animator a steady tick cadence.
    constexpr int k_idle_poll_ms = 16;
    if (const int ret = poll(pfds, 2, k_idle_poll_ms); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "mdi_demo: poll: {}", std::system_category().message(errno));
      break;
    }
    // Advance per-doc animations using the wall clock between
    // iterations. tick_animations() reports active when at least one
    // tween is still running, in which case we keep frame_dirty true
    // so the present loop paints the next frame of the tween.
    const auto now = std::chrono::steady_clock::now();
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick);
    last_tick = now;
    if (shell.tick_animations(dt)) {
      frame_dirty = true;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      if (auto r = input_seat.dispatch(); !r) {
        drm::println(stderr, "mdi_demo: input dispatch: {}", r.error().message());
        break;
      }
    }
    if ((pfds[1].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }

    // ── Cursor servicing ──────────────────────────────────────────
    // Independent of frame_dirty: the sprite tracks the pointer
    // even when no document repaint is pending. Drag-state
    // transitions swap shape (arrow ⇄ grabbing); motion redrains
    // through one move_to per loop iteration so high-rate input
    // doesn't burn one atomic commit per delta.
    if (cursor_renderer) {
      const bool dragging_now = shell.is_dragging();
      if (dragging_now != was_dragging) {
        if (auto r = cursor_renderer->set_cursor(dragging_now ? cursor_grabbing : cursor_default);
            !r) {
          drm::println(stderr, "mdi_demo: cursor set_cursor: {}", r.error().message());
        }
        was_dragging = dragging_now;
        cursor_dirty = true;  // re-commit at current position with the new shape
      }
      if (cursor_dirty) {
        if (auto r =
                cursor_renderer->move_to(static_cast<int>(pointer_x), static_cast<int>(pointer_y));
            !r) {
          drm::println(stderr, "mdi_demo: cursor move_to: {}", r.error().message());
        }
        cursor_dirty = false;
      }
    }

    if (snapshot_requested) {
      snapshot_requested = false;
      if (args.dump_path.empty()) {
        drm::println("mdi_demo: snapshot requested but --dump was not set");
      } else {
        auto snap = drm::capture::snapshot(dev, output->crtc_id);
        if (!snap) {
          drm::println(stderr, "mdi_demo: capture::snapshot: {}", snap.error().message());
        } else if (auto r = drm::capture::write_png(*snap, args.dump_path); !r) {
          drm::println(stderr, "mdi_demo: capture::write_png({}): {}", args.dump_path,
                       r.error().message());
        } else {
          drm::println("mdi_demo: wrote {}", args.dump_path);
        }
      }
    }

    if (!frame_dirty) {
      continue;
    }
    frame_dirty = false;

    // Repaint dirty docs first; geometry-only changes (drag) skip the
    // render path entirely.
    if (auto r = shell.redraw_dirty(); !r) {
      drm::println(stderr, "mdi_demo: redraw_dirty: {}", r.error().message());
      break;
    }

    // Build the per-frame atomic request — overlays only. Bg primary
    // plane state persists across this commit because we never write
    // any of its properties.
    drm::AtomicRequest req(dev);
    if (!req.valid()) {
      drm::println(stderr, "mdi_demo: AtomicRequest ctor failed");
      break;
    }
    const auto refs = shell.surface_refs();
    if (auto r =
            presenter->apply(drm::span<const drm::csd::SurfaceRef>(refs.data(), refs.size()), req);
        !r) {
      drm::println(stderr, "mdi_demo: presenter.apply: {}", r.error().message());
      break;
    }
    flipped = false;
    if (auto r = req.commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
      drm::println(stderr, "mdi_demo: atomic commit: {}", r.error().message());
      break;
    }
    while (!flipped) {
      if (auto r = page_flip.dispatch(-1); !r) {
        drm::println(stderr, "mdi_demo: page_flip.dispatch (frame): {}", r.error().message());
        return EXIT_FAILURE;
      }
    }
  }

  return EXIT_SUCCESS;
}