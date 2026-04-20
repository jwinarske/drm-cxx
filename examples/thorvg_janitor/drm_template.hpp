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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

// POSIX / Linux
#include <dirent.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// DRM
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// ThorVG
#include <thorvg-1/thorvg.h>

// drm-cxx
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>

#include "core/device.hpp"
#include "core/resources.hpp"
#include "input/seat.hpp"
#include "modeset/atomic.hpp"
#include "modeset/mode.hpp"
#include "modeset/page_flip.hpp"

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
inline bool verify(tvg::Result result, std::string failMsg = "") {
  switch (result) {
    case tvg::Result::FailedAllocation:
      std::cout << "FailedAllocation! " << failMsg << std::endl;
      return false;
    case tvg::Result::InsufficientCondition:
      std::cout << "InsufficientCondition! " << failMsg << std::endl;
      return false;
    case tvg::Result::InvalidArguments:
      std::cout << "InvalidArguments! " << failMsg << std::endl;
      return false;
    case tvg::Result::MemoryCorruption:
      std::cout << "MemoryCorruption! " << failMsg << std::endl;
      return false;
    case tvg::Result::NonSupport:
      std::cout << "NonSupport! " << failMsg << std::endl;
      return false;
    case tvg::Result::Unknown:
      std::cout << "Unknown! " << failMsg << std::endl;
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
// — those names are a straight rename from the game's SDL_SCANCODE_* uses
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
  return detail::keystate_storage()[static_cast<std::size_t>(code)];
}

// --------------------------------------------------------------------------
// progress — helper from upstream, used by some demos (kept for parity).
// --------------------------------------------------------------------------
inline float progress(uint32_t elapsed, float durationInSec, bool rewind = false) {
  auto duration = uint32_t(durationInSec * 1000.0f);
  if (elapsed == 0 || duration == 0) {
    return 0.0f;
  }
  bool forward = ((elapsed / duration) % 2 == 0);
  if (elapsed % duration == 0) {
    return forward ? 0.0f : 1.0f;
  }
  auto p = (float(elapsed % duration) / (float)duration);
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

  float timestamp() {
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - start).count();
  }

  void scandir(const char* path) {
    char buf[PATH_MAX];
    auto rpath = realpath(path, buf);
    if (!rpath) {
      return;
    }
    DIR* dir = opendir(rpath);
    if (!dir) {
      std::cout << "Couldn't open directory \"" << rpath << "\"." << std::endl;
      return;
    }
    for (struct dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
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

  uint32_t handles[4] = {b.handle};
  uint32_t pitches[4] = {b.stride};
  uint32_t offsets[4] = {0};
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
  auto* p = static_cast<uint32_t*>(
      mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off_t(mreq.offset)));
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
  auto res = drm::get_resources(fd);
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
    const bool crtc_ok = (plane->possible_crtcs & (1U << crtc_index)) != 0;
    if (crtc_ok) {
      auto* props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
      if (props != nullptr) {
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
inline uint32_t prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char* name) {
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
  static const double half_life = 0.25;
  static auto prev = clock::now();

  auto now = clock::now();
  double dt = std::chrono::duration<double>(now - prev).count();
  prev = now;
  dt = std::max(0.0, std::min(dt, 0.25));
  double alpha = 1.0 - std::exp(-std::log(2.0) * dt / half_life);
  ema_dt += alpha * (dt - ema_dt);
  return static_cast<uint32_t>(1.0 / ema_dt) + 1;
}

}  // namespace detail

// --------------------------------------------------------------------------
// tvgdemo::main — entry point called from tvggame.cpp's int main(). Handles
// DRM device open, modeset, ThorVG init, input seat, and the render loop.
// Returns 0 on clean exit, nonzero on any setup or commit failure.
//
// Signature kept bit-for-bit compatible with upstream template.h so no
// change in the vendored call site is needed. The engine-selector arg1
// (argv[1] == "gl" / "wg") is ignored with a note — we only support
// software raster into a dumb buffer on this backend.
// --------------------------------------------------------------------------
inline int main(Demo* demo, int argc, char** argv, bool clearBuffer = false,
                uint32_t width = 800, uint32_t height = 800, uint32_t threadsCnt = 4,
                bool /*print*/ = false) {
  if (demo == nullptr) {
    drm::println(stderr, "tvgdemo::main: null Demo");
    return EXIT_FAILURE;
  }
  std::unique_ptr<Demo> demo_owner(demo);

  if (argc > 1 && (std::strcmp(argv[1], "gl") == 0 || std::strcmp(argv[1], "wg") == 0)) {
    drm::println("Note: '{}' engine requested; this backend is SW-only, ignoring", argv[1]);
  }

  const char* device_path = "/dev/dri/card0";
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "/dev/dri/", 9) == 0) {
      device_path = argv[i];
    }
  }

  // DRM device
  auto dev_result = drm::Device::open(device_path);
  if (!dev_result) {
    drm::println(stderr, "Failed to open {}", device_path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_result;
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
  const auto mode_res = drm::select_preferred_mode(
      drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes));
  if (!mode_res) {
    drm::println(stderr, "No mode selected");
    return EXIT_FAILURE;
  }
  const auto mode = *mode_res;
  const uint32_t fb_w = mode.width();
  const uint32_t fb_h = mode.height();
  drm::println("Mode: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h, mode.refresh(),
               conn->connector_id, crtc_id);

  // The game asked for a specific (width, height) for its logical canvas; we
  // honor that by rendering the canvas into the full-resolution framebuffer
  // — ThorVG's SwCanvas is told the true stride and dimensions, and the
  // game draws at its coordinate space inside. If the game's size differs
  // from the mode, the extra pixels stay cleared.
  (void)width;
  (void)height;

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
  detail::DumbBuffer buf[2];
  buf[0] = detail::create_dumb(dev.fd(), fb_w, fb_h);
  buf[1] = detail::create_dumb(dev.fd(), fb_w, fb_h);
  if (buf[0].mapped == nullptr || buf[1].mapped == nullptr) {
    drm::println(stderr, "Failed to allocate dumb buffers");
    return EXIT_FAILURE;
  }
  std::memset(buf[0].mapped, 0, buf[0].size);
  std::memset(buf[1].mapped, 0, buf[1].size);

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
  std::unique_ptr<tvg::SwCanvas> canvas(tvg::SwCanvas::gen(tvg::EngineOption(0)));
  if (!canvas) {
    drm::println(stderr, "SwCanvas::gen failed (engine disabled?)");
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }

  // Bind canvas to the initial back buffer.
  int back = 0;
  int front = 1;
  if (!verify(canvas->target(buf[back].mapped, buf[back].stride / 4, fb_w, fb_h,
                             tvg::ColorSpace::ARGB8888))) {
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }

  // Give the demo its initial content and pre-render frame 0 so the first
  // atomic commit displays something real.
  if (!demo_owner->content(canvas.get(), fb_w, fb_h)) {
    drm::println(stderr, "demo.content returned false");
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }
  if (!verify(canvas->draw(clearBuffer)) || !verify(canvas->sync())) {
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }

  auto commit_frame = [&](bool first_frame) -> bool {
    drm::AtomicRequest req(dev);
    auto add = [&](uint32_t obj, uint32_t prop, uint64_t val) {
      auto r = req.add_property(obj, prop, val);
      return r.has_value();
    };
    bool ok = true;
    ok &= add(plane_id, p_fb, buf[back].fb_id);
    ok &= add(plane_id, p_crtc, crtc_id);
    ok &= add(plane_id, p_cx, 0);
    ok &= add(plane_id, p_cy, 0);
    ok &= add(plane_id, p_cw, fb_w);
    ok &= add(plane_id, p_ch, fb_h);
    ok &= add(plane_id, p_sx, 0);
    ok &= add(plane_id, p_sy, 0);
    ok &= add(plane_id, p_sw, uint64_t(fb_w) << 16);
    ok &= add(plane_id, p_sh, uint64_t(fb_h) << 16);
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
    auto r = req.commit(flags);
    if (!r) {
      drm::println(stderr, "atomic commit failed: {}", r.error().message());
      return false;
    }
    return true;
  };

  // libinput seat
  auto seat_res = drm::input::Seat::open();
  if (!seat_res) {
    drm::println(stderr,
                 "Failed to open input seat (need root or 'input' group membership)");
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }
  auto& seat = *seat_res;
  bool quit = false;
  int32_t ptr_x = int32_t(fb_w) / 2;
  int32_t ptr_y = int32_t(fb_h) / 2;
  bool needs_redraw = false;
  seat.set_event_handler([&](const drm::input::InputEvent& event) {
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (ke->key >= 0 && ke->key <= KEY_MAX) {
        detail::keystate_storage()[size_t(ke->key)] = ke->pressed;
      }
      if (ke->key == KEY_ESC && ke->pressed) {
        quit = true;
      }
    } else if (const auto* pe = std::get_if<drm::input::PointerEvent>(&event)) {
      if (const auto* m = std::get_if<drm::input::PointerMotionEvent>(pe)) {
        ptr_x = std::clamp(ptr_x + int32_t(m->dx), 0, int32_t(fb_w) - 1);
        ptr_y = std::clamp(ptr_y + int32_t(m->dy), 0, int32_t(fb_h) - 1);
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

  // PageFlip: mark flip_pending = false on each event so the main loop
  // knows the back buffer that was scanned out is safe to reuse.
  drm::PageFlip page_flip(dev);
  bool flip_pending = false;
  page_flip.set_handler(
      [&](uint32_t /*crtc*/, uint64_t /*seq*/, uint64_t /*ts_ns*/) { flip_pending = false; });

  // First frame: full modeset + page-flip event request
  if (!commit_frame(true)) {
    detail::destroy_dumb(buf[0]);
    detail::destroy_dumb(buf[1]);
    drmModeDestroyPropertyBlob(fd, mode_blob);
    tvg::Initializer::term();
    return EXIT_FAILURE;
  }
  flip_pending = true;
  std::swap(back, front);  // buf[front] is now on-screen; buf[back] is the one we'll draw into
  drm::println("Running — Escape to quit.");

  // Main loop.
  pollfd pfds[2]{};
  pfds[0].fd = seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = fd;
  pfds[1].events = POLLIN;

  const auto loop_start = std::chrono::steady_clock::now();
  uint32_t prev_elapsed = 0;

  while (!quit) {
    // Drain any input + page-flip events with a short timeout so we don't
    // starve the display when the user is idle.
    const int ret = poll(pfds, 2, flip_pending ? -1 : 0);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      if (auto r = seat.dispatch(); !r) {
        drm::println(stderr, "seat.dispatch failed");
        break;
      }
    }
    if ((pfds[1].revents & POLLIN) != 0) {
      (void)page_flip.dispatch(0);
    }
    if (flip_pending) {
      // Still waiting on vblank; don't queue another commit yet.
      continue;
    }

    // Compute elapsed ms since loop start and hand to the game.
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - loop_start).count());
    demo_owner->elapsed = elapsed;
    demo_owner->fps = detail::compute_fps_ema();

    // Ask the game to advance simulation; redraw if it changed (or if an
    // input event already marked the frame dirty). Games that animate
    // every frame (like Janitor) generally return true here.
    const uint32_t diff_ms = elapsed - prev_elapsed;
    prev_elapsed = elapsed;
    (void)diff_ms;
    needs_redraw |= demo_owner->update(canvas.get(), elapsed);

    if (needs_redraw) {
      if (!verify(canvas->target(buf[back].mapped, buf[back].stride / 4, fb_w, fb_h,
                                 tvg::ColorSpace::ARGB8888))) {
        break;
      }
      if (!verify(canvas->draw(clearBuffer)) || !verify(canvas->sync())) {
        break;
      }
      needs_redraw = false;
    }

    if (!commit_frame(false)) {
      break;
    }
    flip_pending = true;
    std::swap(back, front);
  }

  // Teardown order matters: drop canvas before term, detach FBs before
  // destroying the dumb buffers, and release the mode blob last.
  canvas.reset();
  tvg::Initializer::term();
  detail::destroy_dumb(buf[0]);
  detail::destroy_dumb(buf[1]);
  drmModeDestroyPropertyBlob(fd, mode_blob);
  return EXIT_SUCCESS;
}

}  // namespace tvgdemo
