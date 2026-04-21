// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// mouse_cursor — shows a cursor that tracks the mouse via libinput.
//
// Usage: mouse_cursor [--sw] [/dev/dri/cardN]
//
// Tries hardware cursor first. If it fails (or --sw is given), falls back
// to a software cursor rendered via an overlay plane + atomic modesetting.
// Press Escape or Ctrl-C to quit.

#include "../select_device.hpp"
#include "core/device.hpp"
#include "core/property_store.hpp"
#include "core/resources.hpp"
#include "drm-cxx/detail/format.hpp"
#include "input/pointer.hpp"
#include "input/seat.hpp"
#include "session/seat.hpp"

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <variant>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------
namespace {
volatile std::sig_atomic_t g_quit = 0;
}

static void signal_handler(int /*sig*/) {
  g_quit = 1;
}

// ---------------------------------------------------------------------------
// Dumb buffer helpers
// ---------------------------------------------------------------------------
struct DumbBuffer {
  int drm_fd{-1};
  uint32_t handle{};
  uint32_t width{};
  uint32_t height{};
  uint32_t stride{};
  uint64_t size{};
  uint32_t fb_id{};
};

static DumbBuffer create_dumb_buffer(int fd, uint32_t w, const uint32_t h) {
  DumbBuffer buf;
  buf.drm_fd = fd;
  buf.width = w;
  buf.height = h;

  drm_mode_create_dumb create{};
  create.width = w;
  create.height = h;
  create.bpp = 32;

  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
    drm::println(stderr, "Failed to create dumb buffer: {}", std::system_category().message(errno));
    return buf;
  }

  buf.handle = create.handle;
  buf.stride = create.pitch;
  buf.size = create.size;
  return buf;
}

static void destroy_dumb_buffer(DumbBuffer& buf) {
  if (buf.fb_id != 0) {
    drmModeRmFB(buf.drm_fd, buf.fb_id);
    buf.fb_id = 0;
  }
  if (buf.handle != 0) {
    drm_mode_destroy_dumb destroy{};
    destroy.handle = buf.handle;
    ioctl(buf.drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    buf.handle = 0;
  }
}

static bool add_fb(DumbBuffer& buf, uint32_t format) {
  uint32_t handles[4] = {buf.handle};
  uint32_t strides[4] = {buf.stride};
  uint32_t offsets[4] = {0};
  if (drmModeAddFB2(buf.drm_fd, buf.width, buf.height, format, handles, strides, offsets,
                    &buf.fb_id, 0) != 0) {
    drm::println(stderr, "addFB2: {}", std::system_category().message(errno));
    return false;
  }
  return true;
}

static uint32_t* map_dumb_buffer(const DumbBuffer& buf) {
  drm_mode_map_dumb map_req{};
  map_req.handle = buf.handle;
  if (ioctl(buf.drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
    return nullptr;
  }
  auto* ptr = static_cast<uint32_t*>(mmap(nullptr, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                                          buf.drm_fd, static_cast<off_t>(map_req.offset)));
  return (ptr == MAP_FAILED) ? nullptr : ptr;
}

// Classic angled pointer bitmap (12x17), drawn in the top-left corner.
// Both HW and SW paths use this same sprite at the same pixel size.
static constexpr uint32_t kSpriteW = 12;
static constexpr uint32_t kSpriteH = 17;

// clang-format off
static constexpr uint32_t kB = 0xFF000000;  // black
static constexpr uint32_t kW = 0xFFFFFFFF;  // white
static constexpr uint32_t k_ = 0x00000000;  // transparent

static constexpr uint32_t kSprite[kSpriteH][kSpriteW] = {
  {kB,k_,k_,k_,k_,k_,k_,k_,k_,k_,k_,k_},
  {kB,kB,k_,k_,k_,k_,k_,k_,k_,k_,k_,k_},
  {kB,kW,kB,k_,k_,k_,k_,k_,k_,k_,k_,k_},
  {kB,kW,kW,kB,k_,k_,k_,k_,k_,k_,k_,k_},
  {kB,kW,kW,kW,kB,k_,k_,k_,k_,k_,k_,k_},
  {kB,kW,kW,kW,kW,kB,k_,k_,k_,k_,k_,k_},
  {kB,kW,kW,kW,kW,kW,kB,k_,k_,k_,k_,k_},
  {kB,kW,kW,kW,kW,kW,kW,kB,k_,k_,k_,k_},
  {kB,kW,kW,kW,kW,kW,kW,kW,kB,k_,k_,k_},
  {kB,kW,kW,kW,kW,kW,kW,kW,kW,kB,k_,k_},
  {kB,kW,kW,kW,kW,kW,kB,kB,kB,kB,kB,k_},
  {kB,kW,kW,kB,kW,kW,kB,k_,k_,k_,k_,k_},
  {kB,kW,kB,k_,kB,kW,kW,kB,k_,k_,k_,k_},
  {kB,kB,k_,k_,kB,kW,kW,kB,k_,k_,k_,k_},
  {kB,k_,k_,k_,k_,kB,kW,kW,kB,k_,k_,k_},
  {k_,k_,k_,k_,k_,kB,kW,kW,kB,k_,k_,k_},
  {k_,k_,k_,k_,k_,k_,kB,kB,k_,k_,k_,k_},
};
// clang-format on

static void draw_cursor_sprite(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                               uint32_t stride_bytes) {
  const uint32_t stride_px = stride_bytes / 4;
  if (stride_px < buf_w) {
    return;  // sanity check
  }
  for (uint32_t y = 0; y < buf_h; ++y) {
    for (uint32_t x = 0; x < buf_w; ++x) {
      uint32_t color = 0x00000000;
      if (x < kSpriteW && y < kSpriteH) {
        color = kSprite[y][x];
      }
      pixels[y * stride_px + x] = color;
    }
  }
}

// ---------------------------------------------------------------------------
// Find an overlay plane that supports ARGB8888 for the given CRTC
// ---------------------------------------------------------------------------
static uint32_t find_overlay_plane(const int fd, const uint32_t crtc_id) {
  const auto res = drm::get_resources(fd);
  if (!res) return 0;

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

  auto* plane_res = drmModeGetPlaneResources(fd);
  if (!plane_res) {
    return 0;
  }

  uint32_t result = 0;
  for (uint32_t i = 0; i < plane_res->count_planes && result == 0; ++i) {
    auto* plane = drmModeGetPlane(fd, plane_res->planes[i]);
    if (!plane) {
      continue;
    }

    const bool crtc_ok = (plane->possible_crtcs & (1U << crtc_index)) != 0;

    if (const bool in_use = plane->crtc_id != 0; crtc_ok && !in_use) {
      if (auto* props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE)) {
        for (uint32_t j = 0; j < props->count_props; ++j) {
          auto* prop = drmModeGetProperty(fd, props->props[j]);
          if (prop && std::strcmp(prop->name, "type") == 0) {
            if (auto val = props->prop_values[j];
                val == DRM_PLANE_TYPE_OVERLAY || val == DRM_PLANE_TYPE_CURSOR) {
              for (uint32_t f = 0; f < plane->count_formats; ++f) {
                if (plane->formats[f] == DRM_FORMAT_ARGB8888) {
                  result = plane->plane_id;
                  break;
                }
              }
            }
            drmModeFreeProperty(prop);
            break;
          }
          if (prop) {
            drmModeFreeProperty(prop);
          }
        }
        drmModeFreeObjectProperties(props);
      }
    }
    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(plane_res);
  return result;
}

// ---------------------------------------------------------------------------
// Software cursor: position overlay plane via atomic commit
// ---------------------------------------------------------------------------
// Buffer dimensions for the SW cursor. The sprite dimensions (kSpriteW/H)
// are defined above with the bitmap. Many drivers require minimum buffer
// widths (often 64-pixel aligned), so we allocate a larger buffer.
static constexpr uint32_t k_buf_w = 64;  // driver-friendly minimum
static constexpr uint32_t k_buf_h = 64;

static bool sw_cursor_move(int fd, uint32_t plane_id, uint32_t crtc_id, uint32_t fb_id, int x,
                           int y, drm::PropertyStore& props, bool first) {
  auto* req = drmModeAtomicAlloc();
  if (!req) return false;

  auto add = [&](uint32_t obj, const char* name, uint64_t val) -> bool {
    auto prop_id = props.property_id(obj, name);
    if (!prop_id) {
      if (first) drm::println(stderr, "  plane {} missing property '{}'", obj, name);
      return false;
    }
    drmModeAtomicAddProperty(req, obj, *prop_id, val);
    return true;
  };

  add(plane_id, "FB_ID", fb_id);
  add(plane_id, "CRTC_ID", crtc_id);
  add(plane_id, "CRTC_X", static_cast<uint64_t>(x));
  add(plane_id, "CRTC_Y", static_cast<uint64_t>(y));
  // Use the full buffer size — pixels outside the 12x17 sprite are
  // transparent (alpha=0), so the cursor appears the correct size.
  // Using the sprite dimensions directly fails on many drivers due to
  // minimum plane size requirements or lack of sub-buffer SRC support.
  add(plane_id, "CRTC_W", k_buf_w);
  add(plane_id, "CRTC_H", k_buf_h);
  add(plane_id, "SRC_X", 0);
  add(plane_id, "SRC_Y", 0);
  add(plane_id, "SRC_W", static_cast<uint64_t>(k_buf_w) << 16);
  add(plane_id, "SRC_H", static_cast<uint64_t>(k_buf_h) << 16);

  // First commit enabling the plane needs ALLOW_MODESET.
  // Use blocking (synchronous) commits — NONBLOCK causes EBUSY when
  // mouse events arrive faster than the display refresh rate.
  const uint32_t flags = first ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0;

  const int ret = drmModeAtomicCommit(fd, req, flags, nullptr);
  if (ret != 0 && first) {
    drm::println(stderr, "sw_cursor first commit: {}", std::system_category().message(errno));
  }
  drmModeAtomicFree(req);
  return ret == 0;
}

static void sw_cursor_hide(int fd, const uint32_t plane_id, const drm::PropertyStore& props) {
  auto* req = drmModeAtomicAlloc();
  if (req == nullptr) {
    return;
  }
  auto add = [&](const uint32_t obj, const char* name, const uint64_t val) {
    if (const auto prop_id = props.property_id(obj, name)) {
      drmModeAtomicAddProperty(req, obj, *prop_id, val);
    }
  };
  add(plane_id, "FB_ID", 0);
  add(plane_id, "CRTC_ID", 0);
  drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, nullptr);
  drmModeAtomicFree(req);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  // Parse --sw flag before passing to select_device
  bool force_sw = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--sw") == 0) {
      force_sw = true;
      // Remove --sw from argv so select_device doesn't see it
      for (int j = i; j < argc - 1; ++j) {
        argv[j] = argv[j + 1];
      }
      --argc;
      --i;
    }
  }

  const auto path = drm::examples::select_device(argc, argv);
  if (!path) {
    return EXIT_FAILURE;
  }

  // See atomic_modeset for why we claim a seat session.
  auto seat = drm::session::Seat::open();

  // VT-switch state. `pending_resume_fd` carries the fd handed to us by
  // SeatSession's resume callback into the main loop, where the cursor
  // buffer + plane setup is rebuilt outside the libseat dispatch stack.
  bool session_paused = false;
  int pending_resume_fd = -1;

  // Prefer the seat's revocable fd when available; fall back to plain
  // open() otherwise. SeatSession owns the fd; Device::from_fd wraps
  // it without taking over lifetime.
  const auto seat_dev = seat ? seat->take_device(*path) : std::nullopt;
  auto dev_holder = [&]() -> std::optional<drm::Device> {
    if (seat_dev) {
      return drm::Device::from_fd(seat_dev->fd);
    }
    auto r = drm::Device::open(*path);
    if (!r) {
      return std::nullopt;
    }
    return std::move(*r);
  }();
  if (!dev_holder) {
    drm::println(stderr, "Failed to open {}", *path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_holder;

  if (auto r = dev.enable_universal_planes(); !r) {
    drm::println(stderr, "Failed to enable universal planes");
    return EXIT_FAILURE;
  }
  if (auto r = dev.enable_atomic(); !r) {
    drm::println(stderr, "Failed to enable atomic modesetting");
    return EXIT_FAILURE;
  }

  // Find a connected connector with an active CRTC
  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    drm::println(stderr, "Failed to get DRM resources");
    return EXIT_FAILURE;
  }

  uint32_t crtc_id = 0;
  uint32_t mode_w = 0;
  uint32_t mode_h = 0;
  for (int i = 0; i < res->count_connectors; ++i) {
    auto conn = drm::get_connector(dev.fd(), res->connectors[i]);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
      continue;
    }
    if (conn->encoder_id == 0) {
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

  // --- Try hardware cursor (unless --sw) ---
  // hw_cursor_w/h are captured here so the resume handler can re-upload
  // the cursor sprite via drmModeSetCursor with the same dimensions.
  bool hw_cursor = false;
  DumbBuffer cursor_buf{};
  uint32_t hw_cursor_w = 0;
  uint32_t hw_cursor_h = 0;

  if (!force_sw) {
    uint64_t cap_w = 64;
    uint64_t cap_h = 64;
    drmGetCap(dev.fd(), DRM_CAP_CURSOR_WIDTH, &cap_w);
    drmGetCap(dev.fd(), DRM_CAP_CURSOR_HEIGHT, &cap_h);
    hw_cursor_w = static_cast<uint32_t>(cap_w);
    hw_cursor_h = static_cast<uint32_t>(cap_h);

    cursor_buf = create_dumb_buffer(dev.fd(), hw_cursor_w, hw_cursor_h);
    if (cursor_buf.handle != 0) {
      auto* px = map_dumb_buffer(cursor_buf);
      if (px) {
        draw_cursor_sprite(px, hw_cursor_w, hw_cursor_h, cursor_buf.stride);
        munmap(px, cursor_buf.size);
      } else {
        drm::println(stderr, "Failed to map HW cursor buffer");
        destroy_dumb_buffer(cursor_buf);
        cursor_buf = {};
      }
      if (cursor_buf.handle != 0 &&
          drmModeSetCursor(dev.fd(), crtc_id, cursor_buf.handle, hw_cursor_w, hw_cursor_h) == 0) {
        hw_cursor = true;
        drm::println("Using hardware cursor ({}x{})", hw_cursor_w, hw_cursor_h);
      } else {
        drm::println("Hardware cursor unavailable ({}), falling back to overlay...",
                     std::system_category().message(errno));
        destroy_dumb_buffer(cursor_buf);
        cursor_buf = {};
      }
    }
  } else {
    drm::println("--sw flag: skipping hardware cursor");
  }

  // --- Software cursor via overlay plane ---
  uint32_t overlay_plane_id = 0;
  drm::PropertyStore prop_store;

  if (!hw_cursor) {
    overlay_plane_id = find_overlay_plane(dev.fd(), crtc_id);
    if (overlay_plane_id == 0) {
      drm::println(stderr, "No overlay plane available for software cursor");
      return EXIT_FAILURE;
    }

    cursor_buf = create_dumb_buffer(dev.fd(), k_buf_w, k_buf_h);
    if (cursor_buf.handle == 0) {
      return EXIT_FAILURE;
    }

    if (auto* px = map_dumb_buffer(cursor_buf)) {
      draw_cursor_sprite(px, k_buf_w, k_buf_h, cursor_buf.stride);
      munmap(px, cursor_buf.size);
    } else {
      destroy_dumb_buffer(cursor_buf);
      return EXIT_FAILURE;
    }

    if (!add_fb(cursor_buf, DRM_FORMAT_ARGB8888)) {
      destroy_dumb_buffer(cursor_buf);
      return EXIT_FAILURE;
    }

    if (auto r = prop_store.cache_properties(dev.fd(), overlay_plane_id, DRM_MODE_OBJECT_PLANE);
        !r) {
      drm::println(stderr, "Failed to cache plane properties");
      destroy_dumb_buffer(cursor_buf);
      return EXIT_FAILURE;
    }
    drm::println("Using software cursor via overlay plane {}", overlay_plane_id);
  }

  // --- Input ---
  // When a SeatSession is live, route libinput's privileged opens
  // through it so input fds get the same revocation/resume treatment
  // as the DRM fd on VT switch. Otherwise the default opener (direct
  // ::open) keeps the non-seat path working.
  drm::input::InputDeviceOpener input_opener;
  if (seat) {
    input_opener = seat->input_opener();
  }
  auto input_seat_result = drm::input::Seat::open({}, std::move(input_opener));
  if (!input_seat_result) {
    drm::println(stderr, "Failed to open input seat (need root or input group)");
    if (hw_cursor) {
      drmModeSetCursor(dev.fd(), crtc_id, 0, 0, 0);
    }
    if (overlay_plane_id != 0U) {
      sw_cursor_hide(dev.fd(), overlay_plane_id, prop_store);
    }
    destroy_dumb_buffer(cursor_buf);
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_result;

  drm::input::Pointer pointer;
  pointer.reset_position(static_cast<double>(mode_w) / 2.0, static_cast<double>(mode_h) / 2.0);

  bool first_sw_commit = true;
  auto move_cursor = [&](const double cx, const double cy) {
    const int ix = static_cast<int>(cx);
    const int iy = static_cast<int>(cy);
    if (hw_cursor) {
      drmModeMoveCursor(dev.fd(), crtc_id, ix, iy);
    } else {
      sw_cursor_move(dev.fd(), overlay_plane_id, crtc_id, cursor_buf.fb_id, ix, iy, prop_store,
                     first_sw_commit);
      first_sw_commit = false;
    }
  };

  move_cursor(pointer.x(), pointer.y());
  drm::println("Cursor active ({}x{}) — move mouse, Escape to quit", mode_w, mode_h);

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // Track whether the pointer moved this iteration — commit once after
  // all pending events are drained, not on every motion event.
  bool cursor_dirty = false;

  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    if (const auto* pe = std::get_if<drm::input::PointerEvent>(&event)) {
      if (const auto* m = std::get_if<drm::input::PointerMotionEvent>(pe)) {
        pointer.accumulate_motion(m->dx, m->dy);
        cursor_dirty = true;
      } else if (const auto* b = std::get_if<drm::input::PointerButtonEvent>(pe)) {
        pointer.set_button(b->button, b->pressed);
        if (b->pressed) {
          drm::println("Button 0x{:x} at ({:.0f}, {:.0f})", b->button, pointer.x(), pointer.y());
        }
      }
    }
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (ke->key == KEY_ESC && ke->pressed) {
        g_quit = 1;
      }
    }
  });

  // Install seat pause/resume callbacks now that every piece of state
  // the handlers need is live. Resume just stashes the new fd — the
  // full rebuild (dev swap, cursor_buf re-create, HW cursor re-upload
  // or SW overlay re-addFB + re-cache) happens in the main loop below,
  // outside the libseat dispatch call-stack. SeatSession acks the pause
  // internally, so the pause callback just flips the flag and tells
  // libinput to stop using its fds (libinput_suspend closes every
  // device fd via close_restricted, which releases them back to
  // libseat).
  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      (void)input_seat.suspend();
    });
    seat->set_resume_callback([&](std::string_view /*path*/, int new_fd) {
      pending_resume_fd = new_fd;
      session_paused = false;
      // libinput_resume re-opens every device via open_restricted,
      // picking up fresh fds from libseat in the process.
      (void)input_seat.resume();
    });
  }

  pollfd pfds[2]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = seat ? seat->poll_fd() : -1;
  pfds[1].events = POLLIN;

  while (g_quit == 0) {
    int const ret = poll(pfds, 2, 100);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      if (auto r = input_seat.dispatch(); !r) {
        drm::println(stderr, "dispatch failed");
        break;
      }
    }
    if ((pfds[1].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }

    // Post-resume rebuild. The old cursor_buf's GEM handle died with
    // the old fd; for SW cursor the FB_ID and the cached prop_store
    // entries are also dead. Recreate the cursor dumb buffer on the
    // new fd, re-upload the sprite, and re-install it (HW: SetCursor;
    // SW: addFB + cache_properties, and flip first_sw_commit so the
    // next move_cursor commit re-arms every plane property).
    if (pending_resume_fd != -1) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;

      // The old cursor_buf referenced revoked-fd state. Zero the
      // fields we still use (ioctls on the old fd would return
      // -ENODEV; there's no clean way to release those kernel objects
      // and we don't need to — the fd close in logind did it for us).
      cursor_buf = DumbBuffer{};

      dev_holder = drm::Device::from_fd(new_fd);
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed");
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed");
        break;
      }

      const uint32_t buf_w = hw_cursor ? hw_cursor_w : k_buf_w;
      const uint32_t buf_h = hw_cursor ? hw_cursor_h : k_buf_h;
      cursor_buf = create_dumb_buffer(dev.fd(), buf_w, buf_h);
      if (cursor_buf.handle == 0) {
        drm::println(stderr, "resume: cursor buffer realloc failed");
        break;
      }
      if (auto* px = map_dumb_buffer(cursor_buf)) {
        draw_cursor_sprite(px, buf_w, buf_h, cursor_buf.stride);
        munmap(px, cursor_buf.size);
      } else {
        drm::println(stderr, "resume: cursor map failed");
        break;
      }

      if (hw_cursor) {
        if (drmModeSetCursor(dev.fd(), crtc_id, cursor_buf.handle, hw_cursor_w, hw_cursor_h) != 0) {
          drm::println(stderr, "resume: drmModeSetCursor failed ({})",
                       std::system_category().message(errno));
          break;
        }
      } else {
        if (!add_fb(cursor_buf, DRM_FORMAT_ARGB8888)) {
          drm::println(stderr, "resume: addFB2 failed");
          break;
        }
        if (auto r = prop_store.cache_properties(dev.fd(), overlay_plane_id, DRM_MODE_OBJECT_PLANE);
            !r) {
          drm::println(stderr, "resume: cache_properties failed");
          break;
        }
        first_sw_commit = true;
      }
      cursor_dirty = true;  // commit below re-positions the cursor
    }

    // Commit cursor position once per loop iteration, after all pending
    // input events have been drained. This avoids one atomic commit per
    // motion event (mice can report >1000 events/sec). Skipped while
    // paused — we have no master and the commit would fail.
    if (cursor_dirty && !session_paused) {
      const double cx = std::clamp(pointer.x(), 0.0, static_cast<double>(mode_w - 1));
      const double cy = std::clamp(pointer.y(), 0.0, static_cast<double>(mode_h - 1));
      pointer.reset_position(cx, cy);
      move_cursor(cx, cy);
      cursor_dirty = false;
    }
  }

  drm::println("\nRemoving cursor...");
  if (hw_cursor) {
    drmModeSetCursor(dev.fd(), crtc_id, 0, 0, 0);
  } else if (overlay_plane_id != 0) {
    sw_cursor_hide(dev.fd(), overlay_plane_id, prop_store);
  }
  destroy_dumb_buffer(cursor_buf);
  return EXIT_SUCCESS;
}
