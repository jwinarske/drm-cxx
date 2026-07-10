// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// mdi_demo — multi-document desktop on top of drm::csd.
//
// Each "document" is a glass-themed decoration painted by
// drm::csd::Renderer into a drm::csd::Surface and presented either on
// its own reserved DRM overlay plane (drm::csd::PlanePresenter) or
// software-composited onto the primary (drm::csd::CompositePresenter).
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
//            [--dump PATH.png] [--presenter {auto|plane|composite|fb}]
//            [/dev/dri/cardN]
//
// --presenter defaults to `auto`: probe_presenter picks plane (one overlay
// per document) or composite (onto the primary) from the plane budget.
//
// Preconditions match every other csd example: run from a TTY (Ctrl-
// Alt-F3) or a libseat session. Build needs DRM_CXX_HAS_BLEND2D=1.

#include "../common/cursor_size.hpp"
#include "../common/open_output.hpp"
#include "../common/vt_switch.hpp"
#include "core/device.hpp"
#include "csd/overlay_reservation.hpp"
#include "csd/presenter.hpp"
#include "csd/presenter_composite.hpp"
#include "csd/presenter_fb.hpp"
#include "csd/presenter_plane.hpp"
#include "csd/probe_presenter.hpp"
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
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_quit = 0;

void signal_handler(int /*sig*/) {
  g_quit = 1;
}

enum class PresenterMode : std::uint8_t { Auto, Plane, Composite, Fb };

struct Args {
  std::uint32_t initial_docs{2};
  std::string theme_name{"default"};
  std::string dump_path;
  PresenterMode presenter{PresenterMode::Auto};
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

// The KMS object id of the PRIMARY plane bound to this CRTC — the plane
// the Composite presenter scans its canvas out from. 0 when the CRTC
// exposes no PRIMARY (shouldn't happen on a modeset-capable card).
std::uint32_t primary_plane_id_for(const drm::planes::PlaneRegistry& registry,
                                   std::uint32_t crtc_index) {
  for (const auto* cap : registry.for_crtc(crtc_index)) {
    if (cap != nullptr && cap->type == drm::planes::DRMPlaneType::PRIMARY) {
      return cap->id;
    }
  }
  return 0;
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
      if (*v4 == "auto") {
        a.presenter = PresenterMode::Auto;
      } else if (*v4 == "plane") {
        a.presenter = PresenterMode::Plane;
      } else if (*v4 == "composite") {
        a.presenter = PresenterMode::Composite;
      } else if (*v4 == "fb") {
        a.presenter = PresenterMode::Fb;
      } else {
        drm::println(stderr, "mdi_demo: --presenter: unknown value '{}', defaulting to auto", *v4);
      }
      strip(i, n4);
    } else {
      ++i;
    }
  }
  return a;
}

// First /dev/dri/* path on the command line, else the default card. The
// fb path opens this only to allocate decoration surfaces (dumb/GBM); it
// never modesets, so any card with the DUMB cap works.
const char* find_card_path(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]).rfind("/dev/dri/", 0) == 0) {
      return argv[i];
    }
  }
  return "/dev/dri/card0";
}

// --presenter=fb: composite the decorations straight into /dev/fb0. This
// path deliberately does NOT take DRM master (Device::open grabs it, so we
// drop it immediately) — a master would suspend the kernel fbcon and the
// mmap writes would land on an inactive framebuffer. The DRM device is used
// only to allocate the decoration Surfaces; scanout is the fbdev blit.
// Kept separate from the KMS main() so the plane/composite paths (which do
// modeset + atomic commit + a cursor plane) stay untouched.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int run_fb(const Args& args, int argc, char* argv[]) {
  auto pres_res = drm::csd::FramebufferPresenter::create("/dev/fb0");
  if (!pres_res) {
    drm::println(stderr, "mdi_demo: FramebufferPresenter::create(/dev/fb0): {}",
                 pres_res.error().message());
    return EXIT_FAILURE;
  }
  const auto presenter = std::move(*pres_res);
  const std::uint32_t fb_w = presenter->width();
  const std::uint32_t fb_h = presenter->height();
  drm::println("mdi_demo: FramebufferPresenter on /dev/fb0 {}x{} (fourcc 0x{:08x})", fb_w, fb_h,
               presenter->fourcc());

  drm::csd::Theme theme_from_file{};
  const drm::csd::Theme* theme = resolve_theme(args.theme_name, theme_from_file);
  if (theme == nullptr) {
    return EXIT_FAILURE;
  }
  drm::println("mdi_demo: theme = {}", theme->name);

  // Open the card WITHOUT becoming DRM master — Device::open() would call
  // drmSetMaster, which suspends the kernel fbcon and freezes /dev/fb0.
  // A plain open wrapped by from_fd stays a non-master client; CREATE_DUMB
  // and AddFB2 (what Surface allocation needs) don't require master, so the
  // decorations still allocate while fbcon keeps scanning out our blit.
  const char* card = find_card_path(argc, argv);
  const int card_fd =
      ::open(card, O_RDWR | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (card_fd < 0) {
    drm::println(stderr, "mdi_demo: open({}): {}", card, std::system_category().message(errno));
    return EXIT_FAILURE;
  }
  auto dev = drm::Device::from_fd(card_fd);  // non-owning; closed below
  drm::println("mdi_demo: decoration surfaces from {} (non-master)", card);

  std::optional<drm::gbm::GbmDevice> gbm;
  if (auto g = drm::gbm::GbmDevice::create(dev.fd()); g) {
    gbm.emplace(std::move(*g));
  }

  const std::size_t deco_budget = args.initial_docs == 0 ? 1U : args.initial_docs;
  mdi_demo::Shell shell(dev, gbm ? &*gbm : nullptr, *theme, deco_budget);
  for (std::size_t i = 0; i < deco_budget; ++i) {
    if (!shell.spawn_document(fb_w, fb_h)) {
      break;
    }
  }
  drm::println("mdi_demo: spawned {} document{}", shell.document_count(),
               shell.document_count() == 1 ? "" : "s");

  auto input_res = drm::input::Seat::open({});
  if (!input_res) {
    drm::println(stderr, "mdi_demo: input::Seat::open: {} (need root or input group)",
                 input_res.error().message());
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_res;

  double pointer_x = static_cast<double>(fb_w) / 2.0;
  double pointer_y = static_cast<double>(fb_h) / 2.0;
  bool ctrl_left = false;
  bool ctrl_right = false;
  const auto ctrl_held = [&]() { return ctrl_left || ctrl_right; };
  bool frame_dirty = true;

  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (ke->key == KEY_LEFTCTRL) {
        ctrl_left = ke->pressed;
      } else if (ke->key == KEY_RIGHTCTRL) {
        ctrl_right = ke->pressed;
      }
      if (!ke->pressed) {
        return;
      }
      if (ke->key == KEY_ESC || ke->key == KEY_Q) {
        shell.request_quit();
      } else if (ctrl_held() && ke->key == KEY_TAB) {
        shell.cycle_focus();
        frame_dirty = true;
      } else if (ctrl_held() && ke->key == KEY_N) {
        if (shell.spawn_document(fb_w, fb_h)) {
          frame_dirty = true;
        }
      } else if (ctrl_held() && ke->key == KEY_W) {
        shell.close_focused();
        frame_dirty = true;
      }
      return;
    }
    if (const auto* pe = std::get_if<drm::input::PointerEvent>(&event)) {
      if (const auto* m = std::get_if<drm::input::PointerMotionEvent>(pe)) {
        pointer_x = std::clamp(pointer_x + m->dx, 0.0, static_cast<double>(fb_w - 1U));
        pointer_y = std::clamp(pointer_y + m->dy, 0.0, static_cast<double>(fb_h - 1U));
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

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  drm::println("mdi_demo: ready (fb). Drag title bars; Ctrl+N new; Ctrl+W close; Esc quits.");

  // The fb presenter ignores the AtomicRequest (fbdev has no atomic commit),
  // but the Presenter interface takes one; build it once and reuse it.
  drm::AtomicRequest req(dev);
  pollfd pfd{};
  pfd.fd = input_seat.fd();
  pfd.events = POLLIN;
  auto last_tick = std::chrono::steady_clock::now();
  while (g_quit == 0 && !shell.quit_requested()) {
    if (const int ret = poll(&pfd, 1, 16); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "mdi_demo: poll: {}", std::system_category().message(errno));
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick);
    last_tick = now;
    if (shell.tick_animations(dt)) {
      frame_dirty = true;
    }
    if ((pfd.revents & POLLIN) != 0) {
      if (auto r = input_seat.dispatch(); !r) {
        drm::println(stderr, "mdi_demo: input dispatch: {}", r.error().message());
        break;
      }
    }
    if (!frame_dirty) {
      continue;
    }
    frame_dirty = false;
    if (auto r = shell.redraw_dirty(); !r) {
      drm::println(stderr, "mdi_demo: redraw_dirty: {}", r.error().message());
      break;
    }
    const auto refs = shell.surface_refs();
    if (auto r =
            presenter->apply(drm::span<const drm::csd::SurfaceRef>(refs.data(), refs.size()), req);
        !r) {
      drm::println(stderr, "mdi_demo: fb apply: {}", r.error().message());
      break;
    }
  }
  ::close(card_fd);  // Device::from_fd is non-owning
  return EXIT_SUCCESS;
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
  const Args args = parse_args(argc, argv);

  if (args.presenter == PresenterMode::Fb) {
    return run_fb(args, argc, argv);
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

  // ── Presenter (plane-per-decoration or composited canvas) ────────
  // The presenter is the only piece that differs between the two
  // modes; the rest of the demo drives it through the abstract
  // csd::Presenter. `deco_budget` is the max concurrent documents: the
  // plane presenter reserves one overlay per decoration, so it's the
  // reserved count; the composite presenter has no per-decoration
  // plane, so it's the requested `--docs` (all blend onto one canvas).
  std::unique_ptr<drm::csd::Presenter> presenter;
  std::size_t deco_budget = 0;

  // Kept alive for the whole run in Plane mode: the reservation owns
  // the overlay leases the PlanePresenter writes to.
  std::optional<drm::csd::OverlayReservation> reservation_holder;

  // A blue->teal desktop backdrop for the composite presenter (the
  // CompositePresenter copies it, so this local can go out of scope after
  // create()). The plane presenter ignores it — its bg is the LayerScene
  // gradient below.
  std::vector<std::uint8_t> composite_bg(static_cast<std::size_t>(fb_w) * fb_h * 4U);
  paint_bg_gradient(drm::span<std::uint8_t>(composite_bg.data(), composite_bg.size()), fb_w * 4U,
                    fb_w, fb_h);
  const drm::span<const std::uint8_t> bg_span(composite_bg.data(), composite_bg.size());

  if (args.presenter == PresenterMode::Auto) {
    // Let probe_presenter pick: Plane when it can reserve one overlay per
    // document, else Composite onto the primary. (fb isn't a candidate —
    // it needs a non-master device; use --presenter=fb explicitly.)
    drm::csd::ProbeConfig probe_cfg;
    probe_cfg.crtc_id = output->crtc_id;
    probe_cfg.crtc_index = *crtc_idx;
    probe_cfg.desired_decorations = args.initial_docs == 0 ? 1U : args.initial_docs;
    probe_cfg.canvas_width = fb_w;
    probe_cfg.canvas_height = fb_h;
    probe_cfg.plane_base_zpos = primary_zpos_max(registry, *crtc_idx) + 1U;
    probe_cfg.background_argb = bg_span;  // used only if it picks Composite
    auto probed = drm::csd::probe_presenter(dev, registry, probe_cfg);
    if (!probed) {
      drm::println(stderr,
                   "mdi_demo: probe_presenter: {} (no usable KMS plane on CRTC {}; try "
                   "--presenter=fb)",
                   probed.error().message(), output->crtc_id);
      return EXIT_FAILURE;
    }
    presenter = std::move(probed->presenter);
    reservation_holder = std::move(probed->reservation);
    deco_budget = probe_cfg.desired_decorations;
    drm::println("mdi_demo: probe picked {} presenter ({} doc budget)",
                 presenter->tier() == drm::csd::Tier::Plane ? "plane" : "composite", deco_budget);
  } else if (args.presenter == PresenterMode::Composite) {
    const std::uint32_t primary_id = primary_plane_id_for(registry, *crtc_idx);
    if (primary_id == 0U) {
      drm::println(stderr, "mdi_demo: no PRIMARY plane on CRTC {} for the composite canvas",
                   output->crtc_id);
      return EXIT_FAILURE;
    }
    auto presenter_res = drm::csd::CompositePresenter::create(dev, registry, output->crtc_id,
                                                              primary_id, fb_w, fb_h, bg_span);
    if (!presenter_res) {
      drm::println(stderr, "mdi_demo: CompositePresenter::create: {}",
                   presenter_res.error().message());
      return EXIT_FAILURE;
    }
    presenter = std::move(*presenter_res);
    deco_budget = args.initial_docs == 0 ? 1U : args.initial_docs;
    drm::println("mdi_demo: CompositePresenter armed on primary plane {} ({} doc budget)",
                 primary_id, deco_budget);
  } else {
    auto reservation_res = drm::csd::OverlayReservation::create(registry);
    if (!reservation_res) {
      drm::println(stderr, "mdi_demo: OverlayReservation::create: {}",
                   reservation_res.error().message());
      return EXIT_FAILURE;
    }
    reservation_holder.emplace(std::move(*reservation_res));

    // Try to reserve `--docs` overlays for decorations; fall back to
    // smaller counts so the demo runs on plane-budget-limited hardware
    // without forcing the user to guess the limit. The budget defines
    // the upper bound on documents — Ctrl+N stops spawning at that
    // ceiling. A budget of 0 means no compatible overlay exists.
    const auto reserved = reserve_with_fallback(*reservation_holder, *crtc_idx, args.initial_docs);
    if (reserved.empty()) {
      drm::println(stderr,
                   "mdi_demo: no ARGB8888 overlay plane available on CRTC {} — needs at least 1 "
                   "for the decoration. Try a different card, run on hardware with overlay "
                   "support, or use --presenter=composite.",
                   output->crtc_id);
      return EXIT_FAILURE;
    }
    if (reserved.size() < args.initial_docs) {
      drm::println("mdi_demo: --docs={} requested, plane budget {} available; capping",
                   args.initial_docs, reserved.size());
    }
    drm::println("mdi_demo: reserved {} overlay plane{} for decorations", reserved.size(),
                 reserved.size() == 1 ? "" : "s");

    const std::uint64_t base_zpos = primary_zpos_max(registry, *crtc_idx) + 1U;
    auto presenter_res = drm::csd::PlanePresenter::create(
        dev, registry, output->crtc_id,
        drm::span<const std::uint32_t>(reserved.data(), reserved.size()), base_zpos);
    if (!presenter_res) {
      drm::println(stderr, "mdi_demo: PlanePresenter::create: {}", presenter_res.error().message());
      return EXIT_FAILURE;
    }
    presenter = std::move(*presenter_res);
    deco_budget = reserved.size();
    drm::println("mdi_demo: PlanePresenter armed (base_zpos={})", base_zpos);
  }

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
  mdi_demo::Shell shell(dev, gbm ? &*gbm : nullptr, *theme, deco_budget);
  for (std::size_t i = 0; i < deco_budget; ++i) {
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