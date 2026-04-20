// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// mouse_cursor — shows a cursor that tracks the mouse via libinput.
//
// Usage: mouse_cursor [--sw] [--theme NAME] [--cursor NAME] [--size N]
//                    [/dev/dri/cardN]
//
// Loads a cursor from an installed XCursor theme (Adwaita by default) and
// tracks the mouse via libinput. Tries hardware cursor first; if it fails
// (or --sw is given), falls back to a software cursor rendered via an
// overlay plane + atomic modesetting. Press Escape or Ctrl-C to quit.

#include "../select_device.hpp"
#include "core/device.hpp"
#include "core/property_store.hpp"
#include "core/resources.hpp"
#include "drm-cxx/detail/format.hpp"
#include "input/pointer.hpp"
#include "input/seat.hpp"
#include "session/seat.hpp"
#include "xcursor_loader.hpp"

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
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

// Theme fallback chain for cursor loading.
// --theme wins if it resolves; otherwise try two common distro themes,
// then libxcursor's built-in "default" symlink, then let libxcursor pick
// whatever it finds on XCURSOR_PATH. The last two slots are important
// because "default" is a symlink that's present on most systems but can
// point at a theme that doesn't ship the requested cursor name, and
// (nullptr) defers to libxcursor's full search logic.
struct ThemeResult {
  std::optional<LoadedCursor> cursor;
  const char* theme_used{nullptr};  // label only; "(system default)" for nullptr slot
};

// Shapes that middle-click cycles through and digit keys 1..9 jump to.
// Chosen to cover the common intentional shapes (pointers, text, wait) plus
// a couple that illustrate different hotspots (crosshair, grabbing) and
// animation (wait, progress).
static constexpr std::array<const char*, 9> kCycle = {
    "default", "pointer",  "text",        "crosshair", "help",
    "wait",    "progress", "not-allowed", "grabbing",
};

static ThemeResult load_with_fallback(const char* name, int size, const char* user_theme) {
  const char* candidates[] = {
      user_theme, "Bibata-Modern-Classic", "Adwaita", "default", nullptr,
  };
  for (const char* t : candidates) {
    // Skip the user_theme slot when no --theme was given: the trailing
    // nullptr slot already covers libxcursor-defaults, so trying the
    // empty first slot would just be a duplicate call.
    if (t == user_theme && user_theme == nullptr) {
      continue;
    }
    if (auto c = LoadedCursor::load(name, t, size)) {
      return {std::move(c), t != nullptr ? t : "(system default)"};
    }
  }
  return {std::nullopt, nullptr};
}

// Blit a cursor frame into a dumb buffer. Zeroes the whole buffer first so
// stale pixels from any prior frame don't leak around a smaller new frame,
// then centers the frame if it is smaller than the buffer. If the frame is
// larger, it clips at the top-left (the caller's hotspot-offset math must
// account for the zero centering offset in that case).
static void blit_frame(const CursorFrame& f, uint32_t* dst, uint32_t buf_w, uint32_t buf_h,
                       uint32_t stride_bytes) {
  const auto stride_px = static_cast<std::size_t>(stride_bytes / 4);
  for (std::size_t y = 0; y < buf_h; ++y) {
    std::memset(dst + (y * stride_px), 0, static_cast<std::size_t>(buf_w) * 4);
  }

  const uint32_t w = std::min(f.width, buf_w);
  const uint32_t h = std::min(f.height, buf_h);
  const std::size_t x_off = (buf_w > f.width) ? (buf_w - f.width) / 2 : 0;
  const std::size_t y_off = (buf_h > f.height) ? (buf_h - f.height) / 2 : 0;

  for (std::size_t y = 0; y < h; ++y) {
    std::memcpy(dst + ((y + y_off) * stride_px) + x_off,
                &f.pixels[y * static_cast<std::size_t>(f.width)], static_cast<std::size_t>(w) * 4);
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
// Many drivers require minimum buffer widths (often 64-pixel aligned), so the
// SW path allocates at least this size even when the cursor frame is smaller.
static constexpr uint32_t k_sw_min_buf = 64;

static bool sw_cursor_move(int fd, uint32_t plane_id, uint32_t crtc_id, uint32_t fb_id, int x,
                           int y, uint32_t buf_w, uint32_t buf_h, drm::PropertyStore& props,
                           bool first) {
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
  // Use the full buffer size — pixels outside the cursor frame are
  // transparent (alpha=0), so the cursor appears the correct size.
  // Using the frame dimensions directly fails on many drivers due to
  // minimum plane size requirements or lack of sub-buffer SRC support.
  add(plane_id, "CRTC_W", buf_w);
  add(plane_id, "CRTC_H", buf_h);
  add(plane_id, "SRC_X", 0);
  add(plane_id, "SRC_Y", 0);
  add(plane_id, "SRC_W", static_cast<uint64_t>(buf_w) << 16);
  add(plane_id, "SRC_H", static_cast<uint64_t>(buf_h) << 16);

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
  // Pre-parse our flags and strip them from argv so select_device only sees
  // the optional device path.
  bool force_sw = false;
  const char* cli_theme = nullptr;
  const char* cli_cursor = "default";
  int cli_size = 0;  // 0 = use hardware cap (HW) or k_sw_min_buf (SW)

  auto strip = [&](int i, int n) {
    for (int j = i; j + n < argc; ++j) {
      argv[j] = argv[j + n];
    }
    argc -= n;
  };

  for (int i = 1; i < argc;) {
    if (std::strcmp(argv[i], "--sw") == 0) {
      force_sw = true;
      strip(i, 1);
    } else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
      cli_theme = argv[i + 1];
      strip(i, 2);
    } else if (std::strcmp(argv[i], "--cursor") == 0 && i + 1 < argc) {
      cli_cursor = argv[i + 1];
      strip(i, 2);
    } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      cli_size = std::atoi(argv[i + 1]);
      strip(i, 2);
    } else {
      ++i;
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

  // --- Resolve cursor caps (HW path) ---
  // Hardware cursor dimensions are fixed per-driver (DRM_CAP_CURSOR_WIDTH/HEIGHT).
  // SW overlay has no such cap; we just need a driver-friendly minimum.
  uint64_t cap_w = 64;
  uint64_t cap_h = 64;
  if (!force_sw) {
    drmGetCap(dev.fd(), DRM_CAP_CURSOR_WIDTH, &cap_w);
    drmGetCap(dev.fd(), DRM_CAP_CURSOR_HEIGHT, &cap_h);
  }

  // --- Load the cursor from an XCursor theme ---
  int target_size = cli_size;
  if (target_size <= 0) {
    target_size = static_cast<int>(force_sw ? k_sw_min_buf : cap_w);
  }
  auto initial = load_with_fallback(cli_cursor, target_size, cli_theme);
  if (!initial.cursor) {
    drm::println(stderr,
                 "Failed to load cursor '{}' at size {} from any theme (tried {}, "
                 "Bibata-Modern-Classic, Adwaita, default, libxcursor defaults)",
                 cli_cursor, target_size, cli_theme != nullptr ? cli_theme : "(no --theme)");
    return EXIT_FAILURE;
  }
  std::optional<LoadedCursor> current_cursor = std::move(initial.cursor);
  drm::println("Cursors: {} from theme '{}'", cli_cursor, initial.theme_used);
  {
    const CursorFrame& f = current_cursor->first();
    drm::println("Cursor '{}' loaded: {}x{}, hotspot ({}, {}){}", cli_cursor, f.width, f.height,
                 f.xhot, f.yhot, current_cursor->animated() ? " (animated)" : "");
  }

  // Match cli_cursor against kCycle so the first middle-click advances to the
  // next shape in the list rather than restarting from index 0.
  std::size_t current_idx = 0;
  for (std::size_t i = 0; i < kCycle.size(); ++i) {
    if (std::strcmp(kCycle[i], cli_cursor) == 0) {
      current_idx = i;
      break;
    }
  }

  // --- Allocate cursor buffer + mapping ---
  // buf_w/buf_h are captured at outer scope so the VT-switch resume
  // handler can realloc the cursor buffer with the same dimensions.
  bool hw_cursor = false;
  DumbBuffer cursor_buf{};
  uint32_t buf_w = 0;
  uint32_t buf_h = 0;
  uint32_t* mapped = nullptr;

  if (!force_sw) {
    buf_w = static_cast<uint32_t>(cap_w);
    buf_h = static_cast<uint32_t>(cap_h);
    cursor_buf = create_dumb_buffer(dev.fd(), buf_w, buf_h);
    if (cursor_buf.handle != 0) {
      mapped = map_dumb_buffer(cursor_buf);
      if (mapped == nullptr) {
        drm::println(stderr, "Failed to map HW cursor buffer");
        destroy_dumb_buffer(cursor_buf);
        cursor_buf = {};
      } else {
        blit_frame(current_cursor->first(), mapped, buf_w, buf_h, cursor_buf.stride);
        if (drmModeSetCursor(dev.fd(), crtc_id, cursor_buf.handle, buf_w, buf_h) == 0) {
          hw_cursor = true;
          drm::println("Using hardware cursor ({}x{})", buf_w, buf_h);
        } else {
          drm::println("Hardware cursor unavailable ({}), falling back to overlay...",
                       std::system_category().message(errno));
          munmap(mapped, cursor_buf.size);
          mapped = nullptr;
          destroy_dumb_buffer(cursor_buf);
          cursor_buf = {};
        }
      }
    }
  } else {
    drm::println("--sw flag: skipping hardware cursor");
  }

  uint32_t overlay_plane_id = 0;
  drm::PropertyStore prop_store;

  if (!hw_cursor) {
    overlay_plane_id = find_overlay_plane(dev.fd(), crtc_id);
    if (overlay_plane_id == 0) {
      drm::println(stderr, "No overlay plane available for software cursor");
      return EXIT_FAILURE;
    }

    buf_w = std::max(k_sw_min_buf, current_cursor->first().width);
    buf_h = std::max(k_sw_min_buf, current_cursor->first().height);
    cursor_buf = create_dumb_buffer(dev.fd(), buf_w, buf_h);
    if (cursor_buf.handle == 0) {
      return EXIT_FAILURE;
    }

    mapped = map_dumb_buffer(cursor_buf);
    if (mapped == nullptr) {
      destroy_dumb_buffer(cursor_buf);
      return EXIT_FAILURE;
    }
    blit_frame(current_cursor->first(), mapped, buf_w, buf_h, cursor_buf.stride);

    if (!add_fb(cursor_buf, DRM_FORMAT_ARGB8888)) {
      munmap(mapped, cursor_buf.size);
      destroy_dumb_buffer(cursor_buf);
      return EXIT_FAILURE;
    }

    if (auto r = prop_store.cache_properties(dev.fd(), overlay_plane_id, DRM_MODE_OBJECT_PLANE);
        !r) {
      drm::println(stderr, "Failed to cache plane properties");
      munmap(mapped, cursor_buf.size);
      destroy_dumb_buffer(cursor_buf);
      return EXIT_FAILURE;
    }
    drm::println("Using software cursor via overlay plane {} ({}x{} buffer)", overlay_plane_id,
                 buf_w, buf_h);
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
    if (mapped != nullptr) {
      munmap(mapped, cursor_buf.size);
    }
    destroy_dumb_buffer(cursor_buf);
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_result;

  drm::input::Pointer pointer;
  pointer.reset_position(static_cast<double>(mode_w) / 2.0, static_cast<double>(mode_h) / 2.0);

  // Recompute the centering offset each frame: when runtime cursor selection
  // (middle-click or digit keys) swaps in a cursor with different dimensions,
  // the offset and hotspot both change. Keeping this out of a capture keeps
  // the move lambda correct across every shape change.
  bool first_sw_commit = true;
  auto move_cursor = [&](const double cx, const double cy) {
    const CursorFrame& f = current_cursor->first();
    const int x_off = (buf_w > f.width) ? static_cast<int>((buf_w - f.width) / 2) : 0;
    const int y_off = (buf_h > f.height) ? static_cast<int>((buf_h - f.height) / 2) : 0;
    const int ix = static_cast<int>(cx) - (f.xhot + x_off);
    const int iy = static_cast<int>(cy) - (f.yhot + y_off);
    if (hw_cursor) {
      drmModeMoveCursor(dev.fd(), crtc_id, ix, iy);
    } else {
      sw_cursor_move(dev.fd(), overlay_plane_id, crtc_id, cursor_buf.fb_id, ix, iy, buf_w, buf_h,
                     prop_store, first_sw_commit);
      first_sw_commit = false;
    }
  };

  move_cursor(pointer.x(), pointer.y());
  drm::println("Cursor active ({}x{}) — move mouse, middle-click or 1-9 to cycle, Escape to quit",
               mode_w, mode_h);

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // Track whether the pointer moved this iteration — commit once after
  // all pending events are drained, not on every motion event.
  bool cursor_dirty = false;

  // Swap the rendered cursor in place. Shared by the middle-click cycler and
  // the digit-key jumps. The dumb buffer and its mapping stay the same; only
  // the pixels and the logical cursor identity change. On HW we call
  // drmModeSetCursor again with the same handle+size to force the driver to
  // re-read the buffer (same-handle re-uploads are driver-dependent; the
  // README's "What could go wrong" section notes this).
  auto load_and_apply = [&](std::size_t idx) {
    auto r = load_with_fallback(kCycle[idx], target_size, cli_theme);
    if (!r.cursor) {
      drm::println(stderr, "Cursor '{}' not found in any theme", kCycle[idx]);
      return;
    }
    current_cursor = std::move(r.cursor);
    current_idx = idx;
    const CursorFrame& f = current_cursor->first();
    drm::println("Cursor: {} from theme '{}' ({}x{}, hotspot ({}, {})){}", kCycle[idx],
                 r.theme_used, f.width, f.height, f.xhot, f.yhot,
                 current_cursor->animated() ? " [animated]" : "");
    blit_frame(f, mapped, buf_w, buf_h, cursor_buf.stride);
    if (hw_cursor) {
      drmModeSetCursor(dev.fd(), crtc_id, cursor_buf.handle, buf_w, buf_h);
    }
    cursor_dirty = true;
  };

  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    if (const auto* pe = std::get_if<drm::input::PointerEvent>(&event)) {
      if (const auto* m = std::get_if<drm::input::PointerMotionEvent>(pe)) {
        pointer.accumulate_motion(m->dx, m->dy);
        cursor_dirty = true;
      } else if (const auto* b = std::get_if<drm::input::PointerButtonEvent>(pe)) {
        pointer.set_button(b->button, b->pressed);
        if (b->pressed) {
          if (b->button == BTN_MIDDLE) {
            load_and_apply((current_idx + 1) % kCycle.size());
          } else {
            drm::println("Button 0x{:x} at ({:.0f}, {:.0f})", b->button, pointer.x(), pointer.y());
          }
        }
      }
    }
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (ke->pressed) {
        if (ke->key == KEY_ESC) {
          g_quit = 1;
        } else if (ke->key >= KEY_1 && ke->key <= KEY_9) {
          const auto digit = static_cast<std::size_t>(ke->key - KEY_1);
          load_and_apply(std::min(digit, kCycle.size() - 1));
        }
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
      // The old mmap was torn down when the fd closed; drop the pointer
      // without munmap — its size is gone with the old cursor_buf.
      cursor_buf = DumbBuffer{};
      mapped = nullptr;

      dev_holder = drm::Device::from_fd(new_fd);
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed");
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed");
        break;
      }

      // buf_w/buf_h at outer scope retain the dimensions chosen at
      // startup (HW cap for HW cursor, max(min, frame) for SW overlay).
      cursor_buf = create_dumb_buffer(dev.fd(), buf_w, buf_h);
      if (cursor_buf.handle == 0) {
        drm::println(stderr, "resume: cursor buffer realloc failed");
        break;
      }
      mapped = map_dumb_buffer(cursor_buf);
      if (mapped == nullptr) {
        drm::println(stderr, "resume: cursor map failed");
        break;
      }
      blit_frame(current_cursor->first(), mapped, buf_w, buf_h, cursor_buf.stride);

      if (hw_cursor) {
        if (drmModeSetCursor(dev.fd(), crtc_id, cursor_buf.handle, buf_w, buf_h) != 0) {
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
  if (mapped != nullptr) {
    munmap(mapped, cursor_buf.size);
    mapped = nullptr;
  }
  destroy_dumb_buffer(cursor_buf);
  return EXIT_SUCCESS;
}
