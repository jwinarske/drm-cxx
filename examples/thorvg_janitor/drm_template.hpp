// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// drm_template.hpp — DRM/KMS + libinput replacement for the upstream
// SDL-based template.h that ships with ThorVG Janitor. Provides the same
// tvgdemo::{Demo, main} surface the upstream game uses, so tvggame.cpp
// compiles against this header with only a #include swap and a small
// keystate-accessor replacement (two hunks in the game).
//
// Rendering path: ThorVG's SwCanvas writes ARGB8888 into one of two
// dumb buffers owned by a small JanitorBufferSource. The source
// implements drm::scene::LayerBufferSource with an explicit rotation
// setter: the game picks which buffer is "current" before each
// scene.commit(), then swaps. One drm::scene::LayerScene instance owns
// the full plane-assignment + mode-blob + property-ID bookkeeping that
// this file used to do by hand. Input is drm::input::Seat as before;
// keystate is a bool array indexed by KEY_* codes polled via
// tvgdemo::key_pressed().
//
// Session-resume: drm::scene::LayerScene has no rebind() yet (that's
// Phase 2.4), so VT switch tears the whole scene down and rebuilds it
// on the fresh fd. JanitorBufferSource::forget() is called before
// scene teardown so the dumb-buffer destructors don't issue ioctls
// against what is now somebody else's fd.

#pragma once

// C++ standard library
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

// POSIX / Linux
#include <dirent.h>
#include <linux/input-event-codes.h>
#include <poll.h>

// DRM
#include <drm_fourcc.h>
#include <xf86drmMode.h>

// ThorVG
#include <thorvg.h>

// drm-cxx
#include "../common/scene/buffer_source.hpp"
#include "../common/scene/layer_desc.hpp"
#include "../common/scene/layer_scene.hpp"
#include "../common/select_device.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "dumb/buffer.hpp"
#include "input/seat.hpp"
#include "modeset/mode.hpp"
#include "modeset/page_flip.hpp"
#include "session/seat.hpp"

#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Upstream template.h opens these. Preserve them so tvggame.cpp's bare
// `Point`, `cout`, etc. still resolve.
using namespace std;  // NOLINT(google-build-using-namespace)
using namespace tvg;  // NOLINT(google-build-using-namespace)

namespace tvgdemo {

// --------------------------------------------------------------------------
// verify — same shape as upstream so vendored code's verify() calls work
// --------------------------------------------------------------------------
inline bool verify(const tvg::Result result, const std::string& failMsg = "") {
  switch (result) {
    case tvg::Result::FailedAllocation:
      std::cout << "FailedAllocation! " << failMsg << '\n';
      return false;
    case tvg::Result::InsufficientCondition:
      std::cout << "InsufficientCondition! " << failMsg << '\n';
      return false;
    case tvg::Result::InvalidArguments:
      std::cout << "InvalidArguments! " << failMsg << '\n';
      return false;
    case tvg::Result::MemoryCorruption:
      std::cout << "MemoryCorruption! " << failMsg << '\n';
      return false;
    case tvg::Result::NonSupport:
      std::cout << "NonSupport! " << failMsg << '\n';
      return false;
    case tvg::Result::Unknown:
      std::cout << "Unknown! " << failMsg << '\n';
      return false;
    default:
      break;
  }
  return true;
}

// --------------------------------------------------------------------------
// Key state polling. Replaces SDL_GetKeyboardState.
// --------------------------------------------------------------------------
namespace detail {
inline std::array<bool, KEY_MAX + 1>& keystate_storage() {
  static std::array<bool, KEY_MAX + 1> s{};
  return s;
}
}  // namespace detail

[[nodiscard]] inline bool key_pressed(int code) {
  if (code < 0 || code > KEY_MAX) {
    return false;
  }
  return detail::keystate_storage().at(static_cast<std::size_t>(code));
}

// --------------------------------------------------------------------------
// progress — helper from upstream, used by some demos (kept for parity).
// --------------------------------------------------------------------------
inline float progress(const uint32_t elapsed, const float durationInSec,
                      const bool rewind = false) {
  const auto duration = static_cast<uint32_t>(durationInSec * 1000.0F);
  if (elapsed == 0 || duration == 0) {
    return 0.0F;
  }
  const bool forward = ((elapsed / duration) % 2 == 0);
  if (elapsed % duration == 0) {
    return forward ? 0.0F : 1.0F;
  }
  const auto p = (static_cast<float>(elapsed % duration) / static_cast<float>(duration));
  if (rewind) {
    return forward ? p : (1 - p);
  }
  return p;
}

// --------------------------------------------------------------------------
// Demo — virtual base matching upstream's shape exactly.
// --------------------------------------------------------------------------
struct Demo {
  uint32_t elapsed = 0;
  uint32_t fps = 0;

  virtual bool content(tvg::Canvas* canvas, uint32_t w, uint32_t h) = 0;
  virtual bool update(tvg::Canvas* /*canvas*/, uint32_t /*elapsed*/) { return false; }
  virtual bool clickdown(tvg::Canvas* /*canvas*/, int32_t /*x*/, int32_t /*y*/) { return false; }
  virtual bool clickup(tvg::Canvas* /*canvas*/, int32_t /*x*/, int32_t /*y*/) { return false; }
  virtual bool motion(tvg::Canvas* /*canvas*/, int32_t /*x*/, int32_t /*y*/) { return false; }
  virtual void populate(const char* /*path*/) {}
  virtual ~Demo() = default;

  Demo() = default;
  Demo(const Demo&) = delete;
  Demo& operator=(const Demo&) = delete;
  Demo(Demo&&) = delete;
  Demo& operator=(Demo&&) = delete;

  static float timestamp() {
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - start).count();
  }

  void scandir(const char* path) {
    char buf[PATH_MAX];
    const auto* rpath = realpath(path, buf);
    if (rpath == nullptr) {
      return;
    }
    DIR* dir = opendir(rpath);
    if (dir == nullptr) {
      std::cout << "Couldn't open directory \"" << rpath << "\"." << '\n';
      return;
    }
    for (const dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
      if (*entry->d_name == '.' || *entry->d_name == '$') {
        continue;
      }
      if (entry->d_type != DT_DIR) {
        std::string fullpath(path);
        fullpath += '/';
        fullpath += entry->d_name;
        populate(fullpath.c_str());
      }
    }
    closedir(dir);
  }
};

namespace detail {

// --------------------------------------------------------------------------
// JanitorBufferSource — two-buffer LayerBufferSource with explicit
// rotation. drm::scene::DumbBufferSource is intentionally single-
// buffered (no multi-buffer ring in v1), and the Janitor's vsync-aligned
// page-flipping pattern needs a back+front rotation. Rather than extend
// the library-level source speculatively, we keep the ring here; if
// signage_player (Phase 2.6) or a later scene example wants the same
// pattern, promote this to examples/common/scene/.
//
// The "current" index is set by the game before each scene.commit()
// call; acquire() reports the current buffer's fb_id. release() is a
// no-op because the buffers stay resident for the source's full
// lifetime — the game drives rotation via set_current(), so the v1
// acquire/release contract degenerates cleanly into "acquire the buffer
// the game pointed at."
// --------------------------------------------------------------------------
class JanitorBufferSource : public drm::scene::LayerBufferSource {
 public:
  [[nodiscard]] static drm::expected<std::unique_ptr<JanitorBufferSource>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height) {
    drm::dumb::Config cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.drm_format = DRM_FORMAT_ARGB8888;
    cfg.bpp = 32;
    cfg.add_fb = true;

    std::array<drm::dumb::Buffer, 2> bufs;
    for (auto& b : bufs) {
      auto r = drm::dumb::Buffer::create(dev, cfg);
      if (!r) {
        return drm::unexpected<std::error_code>(r.error());
      }
      b = std::move(*r);
    }
    return std::unique_ptr<JanitorBufferSource>(new JanitorBufferSource(std::move(bufs)));
  }

  // ── Pixel access for ThorVG ────────────────────────────────────────
  [[nodiscard]] std::uint32_t* pixels_for(std::size_t idx) noexcept {
    return reinterpret_cast<std::uint32_t*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        buffers_.at(idx).data());
  }
  [[nodiscard]] std::uint32_t stride_px() const noexcept { return buffers_.at(0).stride() / 4; }
  [[nodiscard]] std::uint32_t width() const noexcept { return buffers_.at(0).width(); }
  [[nodiscard]] std::uint32_t height() const noexcept { return buffers_.at(0).height(); }

  void set_current(std::size_t idx) noexcept { current_ = idx; }

  /// Drop all dumb-buffer handles without ioctls. Called before the
  /// owning LayerScene is destroyed on a fd-dead session-resume path,
  /// so the dumb::Buffer destructors don't fire RmFB / DESTROY_DUMB
  /// against what is now a different fd.
  void forget_all() noexcept {
    for (auto& b : buffers_) {
      b.forget();
    }
  }

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override {
    const auto fb = buffers_.at(current_).fb_id();
    if (fb == 0) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    drm::scene::AcquiredBuffer a;
    a.fb_id = fb;
    return a;
  }

  void release(drm::scene::AcquiredBuffer /*acq*/) noexcept override {
    // Buffers stay resident; rotation is driven externally by set_current.
  }

  [[nodiscard]] drm::scene::BindingModel binding_model() const noexcept override {
    return drm::scene::BindingModel::SceneSubmitsFbId;
  }

  [[nodiscard]] drm::scene::SourceFormat format() const noexcept override {
    drm::scene::SourceFormat f;
    f.drm_fourcc = DRM_FORMAT_ARGB8888;
    f.modifier = 0;  // DRM_FORMAT_MOD_LINEAR
    f.width = buffers_.at(0).width();
    f.height = buffers_.at(0).height();
    return f;
  }

 private:
  explicit JanitorBufferSource(std::array<drm::dumb::Buffer, 2> bufs) noexcept
      : buffers_(std::move(bufs)) {}

  std::array<drm::dumb::Buffer, 2> buffers_;
  std::size_t current_{0};
};

// EMA-smoothed FPS counter. Kept as a free function so the game loop
// can report fps without any per-frame state.
inline std::uint32_t compute_fps_ema() {
  using clock = std::chrono::steady_clock;
  static double ema_dt = 1.0 / 60.0;
  static constexpr double half_life = 0.25;
  static auto prev = clock::now();

  const auto now = clock::now();
  double dt = std::chrono::duration<double>(now - prev).count();
  prev = now;
  dt = std::max(0.0, std::min(dt, 0.25));
  const double alpha = 1.0 - std::exp(-std::log(2.0) * dt / half_life);
  ema_dt += alpha * (dt - ema_dt);
  return static_cast<std::uint32_t>(1.0 / ema_dt) + 1;
}

}  // namespace detail

// --------------------------------------------------------------------------
// tvgdemo::main — entry point called from tvggame.cpp's int main().
// Signature matches upstream template.h bit-for-bit; arg[1]'s engine
// selector (gl/wg) is stripped and ignored — this backend is SW-only.
// --------------------------------------------------------------------------
inline int main(Demo* demo, int argc, char** argv, bool clearBuffer = false, uint32_t width = 800,
                uint32_t height = 800, uint32_t threadsCnt = 4, bool /*print*/ = false) {
  if (demo == nullptr) {
    drm::println(stderr, "tvgdemo::main: null Demo");
    return EXIT_FAILURE;
  }
  std::unique_ptr<Demo> demo_owner(demo);

  // Strip upstream engine-selector keywords so select_device only sees
  // a DRM device path (or nothing).
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "gl") == 0 || std::strcmp(argv[i], "wg") == 0 ||
        std::strcmp(argv[i], "sw") == 0) {
      drm::println("Note: '{}' engine requested; this backend is SW-only, ignoring", argv[i]);
      for (int j = i; j < argc - 1; ++j) {
        argv[j] = argv[j + 1];
      }
      --argc;
      --i;
    }
  }

  const auto device_path = drm::examples::select_device(argc, argv);
  if (!device_path) {
    return EXIT_FAILURE;
  }

  // Seat session. When a backend is available, the DRM fd it hands us
  // is revocable on VT switch; the seat dispatches pause/resume
  // callbacks so we can suspend rendering + rebuild KMS state on the
  // fresh fd. No-op (nullopt) when no backend is available.
  auto seat = drm::session::Seat::open();

  // VT-switch state carried across iterations.
  bool session_paused = false;
  bool flip_pending = false;
  int pending_resume_fd = -1;

  // DRM device. Prefer a seat-session fd when available.
  const auto seat_dev = seat ? seat->take_device(*device_path) : std::nullopt;
  auto dev_holder = [&]() -> std::optional<drm::Device> {
    if (seat_dev) {
      return drm::Device::from_fd(seat_dev->fd);
    }
    auto r = drm::Device::open(*device_path);
    if (!r) {
      return std::nullopt;
    }
    return std::move(*r);
  }();
  if (!dev_holder) {
    drm::println(stderr, "Failed to open {}", *device_path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_holder;
  if (auto r = dev.enable_universal_planes(); !r) {
    drm::println(stderr, "enable_universal_planes failed");
    return EXIT_FAILURE;
  }
  if (auto r = dev.enable_atomic(); !r) {
    drm::println(stderr, "enable_atomic failed");
    return EXIT_FAILURE;
  }

  // Connector + CRTC + mode
  auto res = drm::get_resources(dev.fd());
  if (!res) {
    drm::println(stderr, "get_resources failed");
    return EXIT_FAILURE;
  }
  drm::Connector conn{nullptr, &drmModeFreeConnector};
  for (int i = 0; i < res->count_connectors; ++i) {
    if (auto c = drm::get_connector(dev.fd(), res->connectors[i]);
        c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 && c->encoder_id != 0) {
      conn = std::move(c);
      break;
    }
  }
  if (!conn) {
    drm::println(stderr, "No connected connector");
    return EXIT_FAILURE;
  }
  auto enc = drm::get_encoder(dev.fd(), conn->encoder_id);
  if (!enc || enc->crtc_id == 0) {
    drm::println(stderr, "No encoder/CRTC for connector");
    return EXIT_FAILURE;
  }
  const std::uint32_t crtc_id = enc->crtc_id;
  const std::uint32_t connector_id = conn->connector_id;
  const auto mode_res =
      drm::select_preferred_mode(drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes));
  if (!mode_res) {
    drm::println(stderr, "No mode selected");
    return EXIT_FAILURE;
  }
  const auto mode = *mode_res;
  const std::uint32_t fb_w = mode.width();
  const std::uint32_t fb_h = mode.height();
  drm::println("Mode: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h, mode.refresh(),
               connector_id, crtc_id);

  // Scene + source. The scene takes ownership of the source; we keep a
  // raw pointer for pixel-side access (stable for the scene's lifetime
  // since Layer holds the source via unique_ptr, and the Layer stays
  // put until remove_layer or scene teardown).
  auto build_scene = [&](drm::Device& d)
      -> drm::expected<
          std::pair<std::unique_ptr<drm::scene::LayerScene>, detail::JanitorBufferSource*>,
          std::error_code> {
    auto src_res = detail::JanitorBufferSource::create(d, fb_w, fb_h);
    if (!src_res) {
      return drm::unexpected<std::error_code>(src_res.error());
    }
    auto* src_raw = src_res->get();

    drm::scene::LayerScene::Config cfg;
    cfg.crtc_id = crtc_id;
    cfg.connector_id = connector_id;
    cfg.mode = mode.drm_mode;
    auto scene_res = drm::scene::LayerScene::create(d, cfg);
    if (!scene_res) {
      return drm::unexpected<std::error_code>(scene_res.error());
    }
    auto scene = std::move(*scene_res);

    drm::scene::LayerDesc desc;
    desc.source = std::move(*src_res);
    desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
    desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
    desc.content_type = drm::planes::ContentType::UI;
    if (auto r = scene->add_layer(std::move(desc)); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    return std::make_pair(std::move(scene), src_raw);
  };

  auto scene_built = build_scene(dev);
  if (!scene_built) {
    drm::println(stderr, "scene build failed: {}", scene_built.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(scene_built->first);
  auto* source = scene_built->second;

  // Game-requested canvas size, centered in the mode-sized buffer.
  // Pixels outside the canvas stay zero (transparent/black) — the SW
  // raster cost tracks the canvas size rather than the display size.
  const std::uint32_t canvas_w = std::min<std::uint32_t>(width, fb_w);
  const std::uint32_t canvas_h = std::min<std::uint32_t>(height, fb_h);
  const std::uint32_t x_off = (fb_w - canvas_w) / 2;
  const std::uint32_t y_off = (fb_h - canvas_h) / 2;
  const auto canvas_origin_for = [&](std::size_t idx) -> std::uint32_t* {
    // Widen y_off before multiplying; tidy flags uint32_t×uint32_t used
    // as a pointer offset because it would overflow on a 5K+ panel
    // where y_off * stride_px exceeds 2^32.
    return source->pixels_for(idx) + (static_cast<std::size_t>(y_off) * source->stride_px()) +
           x_off;
  };

  // ThorVG init
  if (!verify(tvg::Initializer::init(threadsCnt), "Failed to init ThorVG engine!")) {
    return EXIT_FAILURE;
  }
  std::unique_ptr<tvg::SwCanvas> canvas(tvg::SwCanvas::gen(static_cast<tvg::EngineOption>(0)));
  if (!canvas) {
    drm::println(stderr, "SwCanvas::gen failed (engine disabled?)");
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }

  // Bind canvas to the initial back buffer and pre-render frame 0 so
  // the first scene commit displays something real.
  std::size_t back = 0;
  std::size_t front = 1;
  if (!verify(canvas->target(canvas_origin_for(back), source->stride_px(), canvas_w, canvas_h,
                             tvg::ColorSpace::ARGB8888))) {
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }
  if (!demo_owner->content(canvas.get(), canvas_w, canvas_h)) {
    drm::println(stderr, "demo.content returned false");
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }
  if (!verify(canvas->draw(clearBuffer)) || !verify(canvas->sync())) {
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }

  // PageFlip: clear flip_pending on each event so the loop knows the
  // back buffer is safe to reuse. Commit passes &page_flip as user_data
  // so the kernel routes the completion back through page_flip_handler_v2.
  drm::PageFlip page_flip(dev);
  page_flip.set_handler(
      [&](std::uint32_t /*c*/, std::uint64_t /*s*/, std::uint64_t /*t*/) { flip_pending = false; });

  // libinput seat (distinct from the libseat seat session above).
  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_res = drm::input::Seat::open({}, std::move(libinput_opener));
  if (!input_seat_res) {
    drm::println(stderr, "Failed to open input seat (need root or 'input' group membership)");
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_res;
  bool quit = false;
  std::int32_t ptr_x = static_cast<std::int32_t>(fb_w) / 2;
  std::int32_t ptr_y = static_cast<std::int32_t>(fb_h) / 2;
  bool needs_redraw = false;

  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      flip_pending = false;
      (void)input_seat.suspend();
    });
    seat->set_resume_callback([&](std::string_view /*path*/, int new_fd) {
      pending_resume_fd = new_fd;
      session_paused = false;
      (void)input_seat.resume();
    });
  }
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (ke->key <= KEY_MAX) {
        detail::keystate_storage().at(static_cast<std::size_t>(ke->key)) = ke->pressed;
      }
      if (ke->key == KEY_ESC && ke->pressed) {
        quit = true;
      }
    } else if (const auto* pe = std::get_if<drm::input::PointerEvent>(&event)) {
      if (const auto* m = std::get_if<drm::input::PointerMotionEvent>(pe)) {
        ptr_x = std::clamp(ptr_x + static_cast<std::int32_t>(m->dx), 0,
                           static_cast<std::int32_t>(fb_w) - 1);
        ptr_y = std::clamp(ptr_y + static_cast<std::int32_t>(m->dy), 0,
                           static_cast<std::int32_t>(fb_h) - 1);
        needs_redraw |= demo_owner->motion(canvas.get(), ptr_x, ptr_y);
      } else if (const auto* b = std::get_if<drm::input::PointerButtonEvent>(pe)) {
        if (b->pressed) {
          needs_redraw |= demo_owner->clickdown(canvas.get(), ptr_x, ptr_y);
        } else {
          needs_redraw |= demo_owner->clickup(canvas.get(), ptr_x, ptr_y);
        }
      }
    }
  });

  // First commit — modeset happens implicitly inside the scene (first
  // commit raises DRM_MODE_ATOMIC_ALLOW_MODESET, creates the MODE_ID
  // blob, injects CRTC.MODE_ID/ACTIVE + connector.CRTC_ID).
  source->set_current(back);
  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit failed: {}", r.error().message());
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }
  flip_pending = true;
  std::swap(back, front);
  drm::println("Running — Escape to quit.");

  // Main loop. pfds[2] is the seat-session wakeup fd (present only
  // when a backend was actually claimed); poll ignores negative fds.
  pollfd pfds[3]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;

  const auto loop_start = std::chrono::steady_clock::now();
  std::uint32_t prev_elapsed = 0;

  while (!quit) {
    const int timeout = (flip_pending || session_paused) ? -1 : 0;
    if (const int ret = poll(pfds, 3, timeout); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      if (auto r = input_seat.dispatch(); !r) {
        drm::println(stderr, "input_seat.dispatch failed");
        break;
      }
    }
    if ((pfds[1].revents & POLLIN) != 0) {
      (void)page_flip.dispatch(0);
    }
    if ((pfds[2].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }

    // VT resume: libseat handed us a fresh fd. LayerScene has no
    // rebind() yet (Phase 2.4), so the scene (and everything it owns:
    // mode blob, plane registry, property cache, the source's dumb
    // buffers) is torn down and rebuilt against the new fd. Call
    // source->forget_all() first so the dumb-buffer destructors don't
    // issue ioctls against what is now a different fd.
    if (pending_resume_fd != -1) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;

      source->forget_all();
      scene.reset();
      source = nullptr;

      dev_holder = drm::Device::from_fd(new_fd);
      pfds[1].fd = dev.fd();
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed");
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed");
        break;
      }

      auto rebuilt = build_scene(dev);
      if (!rebuilt) {
        drm::println(stderr, "resume: scene rebuild failed: {}", rebuilt.error().message());
        break;
      }
      scene = std::move(rebuilt->first);
      source = rebuilt->second;

      // PageFlip caches the fd it was constructed against; rebuild so
      // drmHandleEvent targets the new fd.
      page_flip = drm::PageFlip(dev);
      page_flip.set_handler(
          [&](std::uint32_t, std::uint64_t, std::uint64_t) { flip_pending = false; });

      if (!verify(canvas->target(canvas_origin_for(back), source->stride_px(), canvas_w, canvas_h,
                                 tvg::ColorSpace::ARGB8888),
                  "resume canvas rebind")) {
        break;
      }
    }

    if (flip_pending || session_paused) {
      continue;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - loop_start).count());
    demo_owner->elapsed = elapsed;
    demo_owner->fps = detail::compute_fps_ema();

    // Rebind canvas to the buffer we'll draw into this frame BEFORE
    // the game mutates the scene. With threadsCnt > 0, canvas->update()
    // may dispatch work to the threadpool; calling target() afterwards
    // returns InsufficientCondition because the canvas is "performing
    // rendering."
    if (!verify(canvas->target(canvas_origin_for(back), source->stride_px(), canvas_w, canvas_h,
                               tvg::ColorSpace::ARGB8888),
                "target rebind")) {
      break;
    }

    const std::uint32_t diff_ms = elapsed - prev_elapsed;
    prev_elapsed = elapsed;
    (void)diff_ms;
    needs_redraw |= demo_owner->update(canvas.get(), elapsed);

    if (needs_redraw) {
      // Force a clear every frame: we ping-pong two buffers, so the
      // one we're about to paint still holds the scene from two frames
      // ago. ThorVG's scene graph doesn't cover every pixel, so
      // unpainted pixels would retain stale content and ghost.
      (void)clearBuffer;
      if (!verify(canvas->draw(true), "draw") || !verify(canvas->sync(), "sync")) {
        break;
      }
      needs_redraw = false;
    }

    // Tell the source which buffer is current, then commit. The scene
    // takes care of plane binding, property writes, and (on the first
    // commit after a resume-triggered rebuild) the full modeset.
    source->set_current(back);
    if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
        !r) {
      drm::println(stderr, "commit failed: {}", r.error().message());
      break;
    }
    flip_pending = true;
    std::swap(back, front);
  }

  canvas.reset();
  tvg::Initializer::term();
  // scene's destructor releases the layer (and its source, which owns
  // the dumb buffers) and destroys the mode blob. Nothing to do by hand.
  return EXIT_SUCCESS;
}

}  // namespace tvgdemo
