// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// drm_template.hpp — DRM/KMS + libinput replacement for the upstream
// SDL-based template.h that ships with ThorVG Janitor. Provides the same
// tvgdemo::{Demo, main} surface the upstream game uses, so tvggame.cpp
// compiles against this header with only a #include swap and a small
// keystate-accessor replacement (two hunks in the game).
//
// Rendering path: ThorVG's SwCanvas writes ARGB8888 into a dumb buffer
// mmap'd by the program. Two such buffers are created and rotated; at
// each vblank the primary plane's FB_ID is atomically swapped to the
// freshly-drawn back buffer. First frame drives a full modeset (MODE_ID,
// CRTC.ACTIVE, connector.CRTC_ID) with DRM_MODE_ATOMIC_ALLOW_MODESET;
// subsequent frames are plane-only commits with DRM_MODE_PAGE_FLIP_EVENT
// and are non-blocking, gated on drm::PageFlip completion events drained
// via poll(). Input is libinput through drm::input::Seat — keyboard
// presses maintain a Linux KEY_* indexed bool array that tvggame.cpp
// polls via tvgdemo::key_pressed(), pointer motion/button events feed
// the Demo's motion/clickdown/clickup callbacks.
//
// This header is header-only and consumed only by tvggame.cpp. The
// upstream's `using namespace tvg; using namespace std;` convenience is
// preserved intentionally so the vendored game source needs no further
// patching.

#pragma once

// C++ standard library
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

// POSIX / Linux
#include <dirent.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// DRM
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// ThorVG
#include <thorvg.h>

// drm-cxx
#include "../common/select_device.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "input/seat.hpp"
#include "modeset/atomic.hpp"
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
//
// The Linux kernel exposes up to KEY_MAX (~767) input codes, more than we
// need; a bool array sized to KEY_MAX keeps indexing trivial and avoids
// any hashing overhead in the hot path. tvggame.cpp polls this each frame
// via the KEY_A / KEY_UP / etc. constants from <linux/input-event-codes.h>
// — those names are a straight renamed from the game's SDL_SCANCODE_* uses
// (see the 2-hunk patch header in tvggame.cpp).
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
// Demo — virtual base matching upstreams shape exactly.
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

// --------------------------------------------------------------------------
// KmsWindow — drm-cxx's replacement for upstream's SwWindow. Owns the DRM
// device, two dumb buffers, the ThorVG SwCanvas, and the libinput seat.
// Modelled after mouse_cursor/main.cpp's atomic-commit path, but with two
// framebuffers rotated for vsync-aligned page flipping.
// --------------------------------------------------------------------------
namespace detail {

struct DumbBuffer {
  int drm_fd{-1};
  uint32_t handle{};
  uint32_t width{};
  uint32_t height{};
  uint32_t stride{};
  uint64_t size{};
  uint32_t fb_id{};
  uint32_t* mapped{};
};

inline DumbBuffer create_dumb(int fd, uint32_t w, uint32_t h) {
  DumbBuffer b;
  b.drm_fd = fd;
  b.width = w;
  b.height = h;
  drm_mode_create_dumb req{};
  req.width = w;
  req.height = h;
  req.bpp = 32;
  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &req) < 0) {
    drm::println(stderr, "CREATE_DUMB: {}", std::system_category().message(errno));
    return b;
  }
  b.handle = req.handle;
  b.stride = req.pitch;
  b.size = req.size;

  const uint32_t handles[4] = {b.handle};
  const uint32_t pitches[4] = {b.stride};
  constexpr uint32_t offsets[4] = {};
  if (drmModeAddFB2(fd, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &b.fb_id, 0) != 0) {
    drm::println(stderr, "addFB2: {}", std::system_category().message(errno));
    return b;
  }

  drm_mode_map_dumb mreq{};
  mreq.handle = b.handle;
  if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
    drm::println(stderr, "MAP_DUMB: {}", std::system_category().message(errno));
    return b;
  }
  auto* p = static_cast<uint32_t*>(mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                        static_cast<off_t>(mreq.offset)));
  if (p == MAP_FAILED) {
    drm::println(stderr, "mmap: {}", std::system_category().message(errno));
    return b;
  }
  b.mapped = p;
  return b;
}

inline void destroy_dumb(DumbBuffer& b) {
  if (b.mapped != nullptr) {
    munmap(b.mapped, b.size);
    b.mapped = nullptr;
  }
  if (b.fb_id != 0) {
    drmModeRmFB(b.drm_fd, b.fb_id);
    b.fb_id = 0;
  }
  if (b.handle != 0) {
    drm_mode_destroy_dumb dreq{};
    dreq.handle = b.handle;
    ioctl(b.drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    b.handle = 0;
  }
}

// Locate the PRIMARY plane bound (or bindable) to a given CRTC.
inline uint32_t find_primary_plane(int fd, uint32_t crtc_id) {
  const auto res = drm::get_resources(fd);
  if (!res) {
    return 0;
  }
  int crtc_index = -1;
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtc_id) {
      crtc_index = i;
      break;
    }
  }
  if (crtc_index < 0) {
    return 0;
  }
  auto* planes = drmModeGetPlaneResources(fd);
  if (planes == nullptr) {
    return 0;
  }
  uint32_t found = 0;
  for (uint32_t i = 0; i < planes->count_planes && found == 0; ++i) {
    auto* plane = drmModeGetPlane(fd, planes->planes[i]);
    if (plane == nullptr) {
      continue;
    }
    if ((plane->possible_crtcs & (1U << crtc_index)) != 0) {
      if (auto* props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
          props != nullptr) {
        for (uint32_t j = 0; j < props->count_props; ++j) {
          auto* prop = drmModeGetProperty(fd, props->props[j]);
          if (prop != nullptr && std::strcmp(prop->name, "type") == 0 &&
              props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY) {
            found = plane->plane_id;
          }
          if (prop != nullptr) {
            drmModeFreeProperty(prop);
          }
          if (found != 0) {
            break;
          }
        }
        drmModeFreeObjectProperties(props);
      }
    }
    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(planes);
  return found;
}

// Look up a property id by name on a DRM object. Needed for MODE_ID,
// ACTIVE, CRTC_ID, FB_ID, CRTC_X/Y/W/H, SRC_X/Y/W/H on first-frame
// modeset. We don't use PropertyStore here because we only need a handful
// of names across three objects (plane, CRTC, connector).
inline uint32_t prop_id(const int fd, const uint32_t obj_id, const uint32_t obj_type,
                        const char* name) {
  auto* props = drmModeObjectGetProperties(fd, obj_id, obj_type);
  if (props == nullptr) {
    return 0;
  }
  uint32_t out = 0;
  for (uint32_t i = 0; i < props->count_props; ++i) {
    auto* p = drmModeGetProperty(fd, props->props[i]);
    if (p != nullptr) {
      if (std::strcmp(p->name, name) == 0) {
        out = p->prop_id;
      }
      drmModeFreeProperty(p);
    }
    if (out != 0) {
      break;
    }
  }
  drmModeFreeObjectProperties(props);
  return out;
}

inline uint32_t compute_fps_ema() {
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
  return static_cast<uint32_t>(1.0 / ema_dt) + 1;
}

}  // namespace detail

// --------------------------------------------------------------------------
// tvgdemo::main — entry point called from tvggame.cpp's int main(). Handles
// DRM device open, modeset, ThorVG init, input seat, and the render loop.
// Returns 0 on clean exit, nonzero on any setup or commit failure.
//
// Signature kept bit-for-bit compatible with upstream template.h, so no
// change in the vendored call site is needed. The engine-selector arg1
// (argv[1] == "gl" / "wg") is ignored with a note — we only support
// software raster into a dumb buffer on this backend.
// --------------------------------------------------------------------------
inline int main(Demo* demo, int argc, char** argv, bool clearBuffer = false, uint32_t width = 800,
                uint32_t height = 800, uint32_t threadsCnt = 4, bool /*print*/ = false) {
  if (demo == nullptr) {
    drm::println(stderr, "tvgdemo::main: null Demo");
    return EXIT_FAILURE;
  }
  std::unique_ptr<Demo> demo_owner(demo);

  // Strip upstream engine-selector keywords (gl/wg/sw) from argv so
  // drm::examples::select_device only sees a DRM device path (or nothing)
  // and can auto-select or prompt. This backend is SW-only.
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

  // Claim a seat session so that a hard-kill (SIGKILL) of this process
  // triggers the VT switch-back to the text console via the seat
  // provider's session cleanup on connection drop. No-op (returns
  // nullopt) when no seat backend is available.
  auto seat = drm::session::Seat::open();

  // VT-switch pause/resume state. `session_paused` gates rendering while
  // the session is inactive; `needs_modeset` promotes the next commit
  // back to a full modeset (master was dropped); `pending_resume_fd`
  // carries the new DRM fd handed to us by SeatSession's resume
  // callback so the main loop can tear down per-fd state (mode blob,
  // dumb buffers, FB IDs) and rebuild on the fresh fd outside the
  // libseat dispatch call-stack.
  bool session_paused = false;
  bool needs_modeset = false;
  bool flip_pending = false;
  int pending_resume_fd = -1;

  // DRM device. Prefer a revocable seat-session fd when available —
  // the seat provider (logind/seatd/builtin) manages master handoff on
  // VT switch without racing us. Fall back to plain open() when no
  // backend is available. SeatSession owns the fd; Device::from_fd
  // wraps it without taking ownership.
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
  const uint32_t crtc_id = enc->crtc_id;
  const auto mode_res =
      drm::select_preferred_mode(drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes));
  if (!mode_res) {
    drm::println(stderr, "No mode selected");
    return EXIT_FAILURE;
  }
  const auto mode = *mode_res;
  const uint32_t fb_w = mode.width();
  const uint32_t fb_h = mode.height();
  drm::println("Mode: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h, mode.refresh(),
               conn->connector_id, crtc_id);

  // Find primary plane
  const uint32_t plane_id = detail::find_primary_plane(dev.fd(), crtc_id);
  if (plane_id == 0) {
    drm::println(stderr, "No primary plane for CRTC {}", crtc_id);
    return EXIT_FAILURE;
  }

  // Allocate MODE_ID blob once (re-used across first-frame commit)
  uint32_t mode_blob = 0;
  if (drmModeCreatePropertyBlob(dev.fd(), &mode.drm_mode, sizeof(mode.drm_mode), &mode_blob) != 0) {
    drm::println(stderr, "CreatePropertyBlob(MODE): {}", std::system_category().message(errno));
    return EXIT_FAILURE;
  }

  // Two dumb buffers, ARGB8888 at mode resolution
  std::array<detail::DumbBuffer, 2> buf{};
  buf.at(0) = detail::create_dumb(dev.fd(), fb_w, fb_h);
  buf.at(1) = detail::create_dumb(dev.fd(), fb_w, fb_h);
  if (buf.at(0).mapped == nullptr || buf.at(1).mapped == nullptr) {
    drm::println(stderr, "Failed to allocate dumb buffers");
    return EXIT_FAILURE;
  }
  std::memset(buf.at(0).mapped, 0, buf.at(0).size);
  std::memset(buf.at(1).mapped, 0, buf.at(1).size);

  // Honor the game's requested (width, height) as the logical canvas size.
  // We render into that subregion of the mode-sized dumb buffer — centered,
  // with the surrounding pixels left as the zero-fill above. Keeps the SW
  // raster cost proportional to the game's intended resolution rather than
  // the display's; the difference is ~2x pixels on a 3440x1440 panel vs.
  // the game's native 2048x1152, and shows up as frame-rate cliffs on
  // high-particle-count events (e.g. ship impact spawning many Explosions).
  const uint32_t canvas_w = std::min<uint32_t>(width, fb_w);
  const uint32_t canvas_h = std::min<uint32_t>(height, fb_h);
  const uint32_t stride_px = buf.at(0).stride / 4;
  const uint32_t x_off = (fb_w - canvas_w) / 2;
  const uint32_t y_off = (fb_h - canvas_h) / 2;
  const auto canvas_origin = [&](const int idx) -> uint32_t* {
    return buf.at(static_cast<std::size_t>(idx)).mapped + (y_off * stride_px) + x_off;
  };

  // Property ids we will keep pumping every commit
  const int fd = dev.fd();
  const uint32_t p_fb = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
  const uint32_t p_crtc = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
  const uint32_t p_cx = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
  const uint32_t p_cy = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
  const uint32_t p_cw = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
  const uint32_t p_ch = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
  const uint32_t p_sx = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
  const uint32_t p_sy = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
  const uint32_t p_sw = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
  const uint32_t p_sh = detail::prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
  const uint32_t c_active = detail::prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
  const uint32_t c_mode = detail::prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
  const uint32_t n_crtc =
      detail::prop_id(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
  if ((p_fb | p_crtc | p_cx | p_cy | p_cw | p_ch | p_sx | p_sy | p_sw | p_sh | c_active | c_mode |
       n_crtc) == 0) {
    drm::println(stderr, "Failed to resolve one of the required properties");
    return EXIT_FAILURE;
  }

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

  // Bind canvas to the initial back buffer.
  int back = 0;
  int front = 1;
  if (!verify(canvas->target(canvas_origin(back), stride_px, canvas_w, canvas_h,
                             tvg::ColorSpace::ARGB8888))) {
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }

  // Give the demo its initial content and pre-render frame 0 so the first
  // atomic commit displays something real.
  if (!demo_owner->content(canvas.get(), canvas_w, canvas_h)) {
    drm::println(stderr, "demo.content returned false");
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }
  if (!verify(canvas->draw(clearBuffer)) || !verify(canvas->sync())) {
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }

  // PageFlip: mark flip_pending = false on each event so the main loop
  // knows the back buffer that was scanned out is safe to reuse. Declared
  // up here so commit_frame can pass &page_flip as the commit's user_data
  // — libdrm routes that pointer back to page_flip_handler_v2 when the
  // flip completes. `flip_pending` is hoisted above (the pause callback
  // clears it when the session is preempted mid-flight).
  drm::PageFlip page_flip(dev);
  page_flip.set_handler(
      [&](uint32_t /*crtc*/, uint64_t /*seq*/, uint64_t /*ts_ns*/) { flip_pending = false; });

  auto commit_frame = [&](const bool first_frame) -> bool {
    const auto& back_buf = buf.at(static_cast<std::size_t>(back));
    drm::AtomicRequest req(dev);
    auto add = [&](const uint32_t obj, const uint32_t prop, const uint64_t val) {
      const auto r = req.add_property(obj, prop, val);
      return r.has_value();
    };
    bool ok = true;
    ok &= add(plane_id, p_fb, back_buf.fb_id);
    ok &= add(plane_id, p_crtc, crtc_id);
    ok &= add(plane_id, p_cx, 0);
    ok &= add(plane_id, p_cy, 0);
    ok &= add(plane_id, p_cw, fb_w);
    ok &= add(plane_id, p_ch, fb_h);
    ok &= add(plane_id, p_sx, 0);
    ok &= add(plane_id, p_sy, 0);
    ok &= add(plane_id, p_sw, static_cast<uint64_t>(fb_w) << 16);
    ok &= add(plane_id, p_sh, static_cast<uint64_t>(fb_h) << 16);
    if (first_frame) {
      ok &= add(crtc_id, c_mode, mode_blob);
      ok &= add(crtc_id, c_active, 1);
      ok &= add(conn->connector_id, n_crtc, crtc_id);
    }
    if (!ok) {
      return false;
    }
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
    if (first_frame) {
      flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    } else {
      flags |= DRM_MODE_ATOMIC_NONBLOCK;
    }
    if (auto r = req.commit(flags, &page_flip); !r) {
      drm::println(stderr, "atomic commit failed: {}", r.error().message());
      return false;
    }
    return true;
  };

  // libinput seat (distinct from the libseat "seat session" above;
  // this one manages kbd/mouse devices via libinput). If a SeatSession
  // is live, route libinput's privileged opens through it so input fds
  // get the same revocation/resume treatment as the DRM fd on VT
  // switch.
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
  int32_t ptr_x = static_cast<int32_t>(fb_w) / 2;
  int32_t ptr_y = static_cast<int32_t>(fb_h) / 2;
  bool needs_redraw = false;

  // Install seat pause/resume callbacks now that both `flip_pending`
  // and `input_seat` are live. Pause: stop rendering, drop the
  // pending flip state, and tell libinput to release every input fd
  // (libinput_suspend closes them via close_restricted, which hands
  // them back to libseat). Resume: stash the new fd, flag a full
  // modeset, and let libinput re-open inputs to pick up fresh fds.
  // SeatSession acks libseat's pause internally.
  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      flip_pending = false;
      (void)input_seat.suspend();
    });
    seat->set_resume_callback([&](std::string_view /*path*/, int new_fd) {
      pending_resume_fd = new_fd;
      session_paused = false;
      needs_modeset = true;
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
        ptr_x = std::clamp(ptr_x + static_cast<int32_t>(m->dx), 0, static_cast<int32_t>(fb_w) - 1);
        ptr_y = std::clamp(ptr_y + static_cast<int32_t>(m->dy), 0, static_cast<int32_t>(fb_h) - 1);
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

  // First frame: full modeset + page-flip event request
  if (!commit_frame(true)) {
    detail::destroy_dumb(buf.at(0));
    detail::destroy_dumb(buf.at(1));
    drmModeDestroyPropertyBlob(dev.fd(), mode_blob);
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }
  flip_pending = true;
  std::swap(back, front);  // buf[front] is now on-screen; buf[back] is the one we'll draw into
  drm::println("Running — Escape to quit.");

  // Main loop. Third slot is the seat-session wakeup fd (present only
  // when a seat backend was actually claimed); it stays -1 in the
  // fallback path, and poll() ignores negative fds. Slot 1 (the DRM
  // fd) is re-pointed on resume when the seat hands us a fresh fd.
  pollfd pfds[3]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;

  const auto loop_start = std::chrono::steady_clock::now();
  uint32_t prev_elapsed = 0;

  while (!quit) {
    // Block indefinitely while either a flip is in flight or the session
    // is paused — we have nothing useful to do until an event arrives
    // (flip completion, input, or a seat resume). Otherwise poll
    // non-blocking so the render cadence stays driven by the clock.
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
      // Drains pause/resume signals; the callbacks update
      // session_paused / needs_modeset / flip_pending /
      // pending_resume_fd synchronously.
      seat->dispatch();
    }

    // After a ResumeDevice signal we own a fresh DRM fd. Every piece
    // of per-fd state tied to the old (now revoked) fd is dead —
    // framebuffer registrations, dumb buffer handles, the MODE_ID
    // property blob, and the mmap'd regions (the backing GEM was
    // released when the fd closed). Rebuild all of that here, outside
    // the DBus dispatch call-stack, so errors propagate cleanly and
    // the canvas isn't mid-draw. KMS object IDs (plane, crtc,
    // connector) and property IDs are per-device and stay stable, so
    // those don't need re-resolving.
    if (pending_resume_fd != -1) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;

      for (auto& b : buf) {
        if (b.mapped != nullptr) {
          munmap(b.mapped, b.size);
          b.mapped = nullptr;
        }
        b.handle = 0;
        b.fb_id = 0;
        b.drm_fd = -1;
      }
      mode_blob = 0;

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

      if (drmModeCreatePropertyBlob(dev.fd(), &mode.drm_mode, sizeof(mode.drm_mode), &mode_blob) !=
          0) {
        drm::println(stderr, "resume: CreatePropertyBlob(MODE): {}",
                     std::system_category().message(errno));
        break;
      }

      buf.at(0) = detail::create_dumb(dev.fd(), fb_w, fb_h);
      buf.at(1) = detail::create_dumb(dev.fd(), fb_w, fb_h);
      if (buf.at(0).mapped == nullptr || buf.at(1).mapped == nullptr) {
        drm::println(stderr, "resume: dumb buffer realloc failed");
        break;
      }
      std::memset(buf.at(0).mapped, 0, buf.at(0).size);
      std::memset(buf.at(1).mapped, 0, buf.at(1).size);

      // PageFlip caches the fd it was constructed against; rebuild so
      // drmHandleEvent targets the new fd. Re-install the handler —
      // move-assign drops the previous one.
      page_flip = drm::PageFlip(dev);
      page_flip.set_handler(
          [&](uint32_t /*crtc*/, uint64_t /*seq*/, uint64_t /*ts_ns*/) { flip_pending = false; });

      if (!verify(canvas->target(canvas_origin(back), stride_px, canvas_w, canvas_h,
                                 tvg::ColorSpace::ARGB8888),
                  "resume canvas rebind")) {
        break;
      }
      // needs_modeset was set by the resume callback — first commit
      // after this point rides the first_frame=true path.
    }

    if (flip_pending || session_paused) {
      // Either waiting on vblank or the session is inactive — in both
      // cases don't queue another commit. Resume will flip
      // session_paused back and the next iteration proceeds.
      continue;
    }

    // Compute elapsed ms since loop start and hand to the game.
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - loop_start).count());
    demo_owner->elapsed = elapsed;
    demo_owner->fps = detail::compute_fps_ema();

    // Rebind the canvas to the buffer we'll draw into this frame
    // BEFORE the game mutates the scene and calls canvas->update().
    // With threadsCnt > 0, canvas->update() may dispatch work to the
    // threadpool; calling target() afterwards returns
    // InsufficientCondition because the canvas is still "performing
    // rendering". Since we sync'd before the previous commit, the
    // canvas is idle here and target() is safe.
    if (!verify(canvas->target(canvas_origin(back), stride_px, canvas_w, canvas_h,
                               tvg::ColorSpace::ARGB8888),
                "target rebind")) {
      break;
    }

    // Ask the game to advance simulation; redraw if it changed (or if an
    // input event already marked the frame dirty). Games that animate
    // every frame (like Janitor) generally return true here.
    const uint32_t diff_ms = elapsed - prev_elapsed;
    prev_elapsed = elapsed;
    (void)diff_ms;
    needs_redraw |= demo_owner->update(canvas.get(), elapsed);

    if (needs_redraw) {
      // Force a clear every frame: we ping-pong two DRM buffers, so the
      // back buffer we're about to paint still holds the scene from two
      // frames ago. ThorVG's scene graph doesn't cover every pixel of
      // the canvas, so any pixel the scene doesn't explicitly paint
      // would retain that stale content and show as ghosting. The
      // upstream-facing clearBuffer arg doesn't know about our swap
      // chain — override it here.
      (void)clearBuffer;
      if (!verify(canvas->draw(true), "draw") || !verify(canvas->sync(), "sync")) {
        break;
      }
      needs_redraw = false;
    }

    // After a seat resume, master was dropped — re-seed the full
    // modeset (MODE_ID + CRTC.ACTIVE + connector.CRTC_ID) on this
    // commit, then revert to plane-only page flips.
    const bool do_modeset = needs_modeset;
    needs_modeset = false;
    if (!commit_frame(do_modeset)) {
      break;
    }
    flip_pending = true;
    std::swap(back, front);
  }

  // Teardown order matters: drop canvas before term, detach FBs before
  // destroying the dumb buffers, and release the mode blob last. Use
  // `dev.fd()` rather than the cached `fd` so resume-swapped fds are
  // honored. Each DumbBuffer carries its own fd (captured at
  // create_dumb), so destroy_dumb is automatically correct across
  // resume.
  canvas.reset();
  tvg::Initializer::term();
  detail::destroy_dumb(buf.at(0));
  detail::destroy_dumb(buf.at(1));
  if (mode_blob != 0) {
    drmModeDestroyPropertyBlob(dev.fd(), mode_blob);
  }
  return EXIT_SUCCESS;
}

}  // namespace tvgdemo
