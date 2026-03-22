// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// mouse_cursor — shows a hardware cursor that tracks the mouse via libinput.
//
// Usage: mouse_cursor [/dev/dri/cardN]
//
// Opens a DRM device, finds a connected connector with an active CRTC,
// creates a dumb buffer with a simple arrow sprite, and moves the hardware
// cursor in response to libinput pointer events. Press Escape or Ctrl-C
// to quit.

#include "../select_device.hpp"
#include "core/device.hpp"
#include "core/resources.hpp"
#include "input/pointer.hpp"
#include "input/seat.hpp"
#include "modeset/mode.hpp"

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <print>
#include <span>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
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

static DumbBuffer create_dumb_buffer(int fd, uint32_t w, uint32_t h) {
  DumbBuffer buf;
  buf.drm_fd = fd;
  buf.width = w;
  buf.height = h;

  drm_mode_create_dumb create{};
  create.width = w;
  create.height = h;
  create.bpp = 32;

  if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
    std::println(stderr, "Failed to create dumb buffer: {}", std::system_category().message(errno));
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

// Draw a simple white arrow cursor into a 32x32 ARGB buffer.
static void draw_cursor_sprite(uint32_t* pixels, uint32_t w, uint32_t h, uint32_t stride_bytes) {
  const uint32_t stride_px = stride_bytes / 4;

  // Clear to transparent
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      pixels[y * stride_px + x] = 0x00000000;
    }
  }

  // Arrow shape: each row defines the filled pixel range.
  // Simple triangular pointer with a border.
  //   Row 0: x=[0,1)   (tip)
  //   Row 1: x=[0,2)
  //   ...
  //   Row N: x=[0,N+1) up to a max width, then taper for the tail.
  const uint32_t arrow_h = std::min(h, 24u);
  const uint32_t body_h = std::min(arrow_h, 16u);

  auto set = [&](uint32_t x, uint32_t y, uint32_t color) {
    if (x < w && y < h) {
      pixels[y * stride_px + x] = color;
    }
  };

  const uint32_t white = 0xFFFFFFFF;
  const uint32_t black = 0xFF000000;

  // Triangular body (rows 0..body_h)
  for (uint32_t row = 0; row < body_h; ++row) {
    uint32_t fill_w = (row / 2) + 1;
    // Black outline left edge
    set(0, row, black);
    // White fill
    for (uint32_t x = 1; x < fill_w; ++x) {
      set(x, row, white);
    }
    // Black outline right edge / bottom diagonal
    set(fill_w, row, black);
    // Black outline bottom
    if (row == body_h - 1) {
      for (uint32_t x = 0; x <= fill_w; ++x) {
        set(x, row, black);
      }
    }
  }

  // Narrow tail (rows body_h..arrow_h)
  for (uint32_t row = body_h; row < arrow_h; ++row) {
    set(0, row, black);
    set(1, row, white);
    set(2, row, white);
    set(3, row, black);
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(const int argc, char* argv[]) {
  const auto path = drm::examples::select_device(argc, argv);
  if (!path) {
    return EXIT_FAILURE;
  }

  // Open DRM device
  auto dev_result = drm::Device::open(*path);
  if (!dev_result) {
    std::println(stderr, "Failed to open {}", *path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_result;

  // Enable capabilities
  if (const auto r = dev.enable_universal_planes(); !r) {
    std::println(stderr, "Failed to enable universal planes");
    return EXIT_FAILURE;
  }
  if (const auto r = dev.enable_atomic(); !r) {
    std::println(stderr, "Failed to enable atomic modesetting");
    return EXIT_FAILURE;
  }

  // Get resources and find a connected connector with an active CRTC
  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    std::println(stderr, "Failed to get DRM resources");
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

    // Get the current mode from the CRTC
    auto crtc = drm::get_crtc(dev.fd(), enc->crtc_id);
    if (!crtc || !crtc->mode_valid) {
      continue;
    }

    crtc_id = enc->crtc_id;
    mode_w = crtc->mode.hdisplay;
    mode_h = crtc->mode.vdisplay;

    std::println("Using connector {} on CRTC {} ({}x{})", conn->connector_id, crtc_id, mode_w,
                 mode_h);
    break;
  }

  if (crtc_id == 0) {
    std::println(stderr, "No connected connector with an active CRTC found");
    return EXIT_FAILURE;
  }

  // Create cursor buffer (64x64 is the standard cursor size for most hardware)
  constexpr uint32_t cursor_w = 64;
  constexpr uint32_t cursor_h = 64;

  auto cursor_buf = create_dumb_buffer(dev.fd(), cursor_w, cursor_h);
  if (cursor_buf.handle == 0) {
    return EXIT_FAILURE;
  }

  // Map and draw the cursor sprite
  {
    drm_mode_map_dumb map_req{};
    map_req.handle = cursor_buf.handle;
    if (ioctl(dev.fd(), DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
      std::println(stderr, "Failed to map dumb buffer: {}", std::system_category().message(errno));
      destroy_dumb_buffer(cursor_buf);
      return EXIT_FAILURE;
    }

    auto* map = static_cast<uint32_t*>(mmap(nullptr, cursor_buf.size, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, dev.fd(), map_req.offset));
    if (map == MAP_FAILED) {
      std::println(stderr, "mmap failed: {}", std::system_category().message(errno));
      destroy_dumb_buffer(cursor_buf);
      return EXIT_FAILURE;
    }

    draw_cursor_sprite(map, cursor_w, cursor_h, cursor_buf.stride);
    munmap(map, cursor_buf.size);
  }

  // Set the cursor on the CRTC
  if (drmModeSetCursor(dev.fd(), crtc_id, cursor_buf.handle, cursor_w, cursor_h) != 0) {
    std::println(stderr, "drmModeSetCursor failed: {}", std::system_category().message(errno));
    destroy_dumb_buffer(cursor_buf);
    return EXIT_FAILURE;
  }

  // Open libinput seat
  auto seat_result = drm::input::Seat::open();
  if (!seat_result) {
    std::println(stderr, "Failed to open input seat (need root or input group)");
    drmModeSetCursor(dev.fd(), crtc_id, 0, 0, 0);
    destroy_dumb_buffer(cursor_buf);
    return EXIT_FAILURE;
  }
  auto& seat = *seat_result;

  // Pointer state — start at screen center
  drm::input::Pointer pointer;
  pointer.reset_position(static_cast<double>(mode_w) / 2.0, static_cast<double>(mode_h) / 2.0);

  // Move cursor to initial position
  drmModeMoveCursor(dev.fd(), crtc_id, static_cast<int>(pointer.x()),
                    static_cast<int>(pointer.y()));

  std::println("Mouse cursor active ({}x{}) — move the mouse, press Escape to quit", mode_w,
               mode_h);

  // Install signal handler
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // Event handler
  seat.set_event_handler([&](const drm::input::InputEvent& event) {
    // Handle pointer events
    if (auto* ptr_ev = std::get_if<drm::input::PointerEvent>(&event)) {
      if (auto* motion = std::get_if<drm::input::PointerMotionEvent>(ptr_ev)) {
        pointer.accumulate_motion(motion->dx, motion->dy);

        // Clamp to screen bounds
        double cx = std::clamp(pointer.x(), 0.0, static_cast<double>(mode_w - 1));
        double cy = std::clamp(pointer.y(), 0.0, static_cast<double>(mode_h - 1));
        pointer.reset_position(cx, cy);

        drmModeMoveCursor(dev.fd(), crtc_id, static_cast<int>(cx), static_cast<int>(cy));
      } else if (auto* btn = std::get_if<drm::input::PointerButtonEvent>(ptr_ev)) {
        pointer.set_button(btn->button, btn->pressed);
        if (btn->pressed) {
          std::println("Button 0x{:x} pressed at ({:.0f}, {:.0f})", btn->button, pointer.x(),
                       pointer.y());
        }
      }
    }

    // Handle Escape key to quit
    if (auto* key_ev = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (key_ev->key == KEY_ESC && key_ev->pressed) {
        g_quit = 1;
      }
    }
  });

  // Main loop: poll on libinput fd
  pollfd pfd{};
  pfd.fd = seat.fd();
  pfd.events = POLLIN;

  while (!g_quit) {
    int ret = poll(&pfd, 1, 100);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::println(stderr, "poll failed: {}", std::system_category().message(errno));
      break;
    }
    if (ret > 0 && (pfd.revents & POLLIN)) {
      if (auto r = seat.dispatch(); !r) {
        std::println(stderr, "seat.dispatch failed");
        break;
      }
    }
  }

  // Cleanup: remove cursor
  std::println("\nRemoving cursor...");
  drmModeSetCursor(dev.fd(), crtc_id, 0, 0, 0);
  destroy_dumb_buffer(cursor_buf);

  return EXIT_SUCCESS;
}
