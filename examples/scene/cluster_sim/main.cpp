// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cluster_sim — automotive instrument-cluster showcase. Skeleton stage:
// a single full-screen Blend2D-painted backdrop (radial gradient over
// dark blue, the conventional cluster aesthetic) plus the standard
// libseat session + libinput keyboard + VT-switch boilerplate. No
// dials, no readouts, no warning indicators yet — those land in
// follow-up commits.
//
// What the finished demo will exercise (per docs/roadmap.md Phase 6.2
// follow-up):
//   * Multi-layer scene composition (bg + speedometer + tachometer +
//     center info + warning overlay + optional rear-view video).
//   * Format heterogeneity (ARGB8888 dial layers over an XRGB8888 bg,
//     plus eventual NV12 video via V4l2DecoderSource).
//   * Priority-driven plane allocation (warning overlay must hit a
//     hardware plane regardless of how many dials had to composite).
//   * Realtime per-frame Blend2D paint for animated dial sweeps.
//
// What this commit ships:
//   * Build wiring (CMake + Meson) under the existing Blend2D gate.
//   * The scene-example boilerplate: open output, libseat session,
//     libinput keyboard, VT-switch chord forwarding, page-flip event
//     loop, session pause/resume that reapplies the bg paint against
//     the new fd.
//   * A solid skeleton that runs to a commit, holds it on screen, and
//     exits cleanly on Esc / Q.
//
// Key bindings (skeleton):
//   Esc / q / Ctrl-C — quit.
//   Ctrl+Alt+F<n>    — VT switch (forwarded to libseat).

#include "../../common/open_output.hpp"
#include "../../common/vt_switch.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

// Blend2D ships its umbrella header at <blend2d/blend2d.h> on the
// upstream source install + Fedora; older Debian/Ubuntu packages put
// it at <blend2d.h>. Cover both via __has_include, mirroring the
// project's existing pattern in capture/snapshot.cpp.
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#else
#error "Blend2D header not found despite the build's blend2d gate being on"
#endif

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <variant>

namespace {

// Cluster-aesthetic palette. Center is near-black with a subtle blue
// cast (matches the "deep instrument" look most automotive clusters
// settle on); edges fade darker so the dial layers landing in step 2
// have somewhere to read against.
constexpr std::uint32_t k_bg_center_argb = 0xFF101626U;
constexpr std::uint32_t k_bg_edge_argb = 0xFF02040AU;

// Paint a radial gradient into an XRGB8888 dumb buffer. Blend2D
// initializes a BLImage view over the existing pixel buffer (no copy)
// and the BLContext flushes on `end()` before we return.
//
// NOLINTBEGIN(misc-include-cleaner) — Blend2D types reach this TU
// through the <blend2d/blend2d.h> umbrella; include-cleaner can't
// resolve the per-symbol header given the umbrella's macro guards.
// Same suppression as signage_player/overlay_renderer.cpp.
void paint_bg_gradient(drm::BufferMapping& mapping, std::uint32_t width,
                       std::uint32_t height) noexcept {
  if (width == 0U || height == 0U) {
    return;
  }
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(height) * stride) {
    return;
  }

  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(width), static_cast<int>(height), BL_FORMAT_XRGB32,
                              pixels.data(), static_cast<intptr_t>(stride), BL_DATA_ACCESS_RW,
                              nullptr, nullptr) != BL_SUCCESS) {
    return;
  }

  BLContext ctx(canvas);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);

  double const cx = static_cast<double>(width) / 2.0;
  double const cy = static_cast<double>(height) / 2.0;
  double const radius = static_cast<double>(width > height ? width : height) * 0.6;
  BLGradient grad(BLRadialGradientValues(cx, cy, cx, cy, radius));
  grad.add_stop(0.0, BLRgba32(k_bg_center_argb));
  grad.add_stop(1.0, BLRgba32(k_bg_edge_argb));
  ctx.fill_all(grad);
  ctx.end();
}
// NOLINTEND(misc-include-cleaner)

}  // namespace

int main(int argc, char** argv) {
  auto ctx_opt = drm::examples::open_and_pick_output(argc, argv);
  if (!ctx_opt.has_value()) {
    return EXIT_FAILURE;
  }
  auto& ctx = *ctx_opt;
  auto& dev = ctx.device;
  auto& seat = ctx.seat;
  std::uint32_t const fb_w = ctx.mode.hdisplay;
  std::uint32_t const fb_h = ctx.mode.vdisplay;

  // Bg layer — painted once, scanned out forever (until step 2 starts
  // mutating per-frame state on the dial layers above it).
  auto bg_src_r = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src_r) {
    drm::println(stderr, "DumbBufferSource::create (bg): {}", bg_src_r.error().message());
    return EXIT_FAILURE;
  }
  auto bg_src = std::move(*bg_src_r);
  auto* bg_src_raw = bg_src.get();
  if (auto m = bg_src->map(drm::MapAccess::Write); m) {
    paint_bg_gradient(*m, fb_w, fb_h);
  } else {
    drm::println(stderr, "bg paint: map failed: {}", m.error().message());
    return EXIT_FAILURE;
  }

  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = ctx.crtc_id;
  scene_cfg.connector_id = ctx.connector_id;
  scene_cfg.mode = ctx.mode;
  auto scene_r = drm::scene::LayerScene::create(dev, scene_cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(bg_src);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  if (auto h = scene->add_layer(std::move(bg_desc)); !h) {
    drm::println(stderr, "add_layer (bg): {}", h.error().message());
    return EXIT_FAILURE;
  }

  bool flip_pending = false;
  drm::PageFlip page_flip(dev);
  page_flip.set_handler(
      [&](std::uint32_t /*c*/, std::uint64_t /*s*/, std::uint64_t /*t*/) { flip_pending = false; });

  // libinput keyboard + VT-switch chord forwarding. libseat puts the
  // TTY in KD_GRAPHICS where the kernel suppresses Ctrl-C signal
  // generation, so the libinput keyboard is the only reliable in-app
  // quit path on a real VT.
  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_r = drm::input::Seat::open({}, std::move(libinput_opener));
  if (!input_seat_r) {
    drm::println(stderr, "drm::input::Seat::open: {} (need root or 'input' group membership)",
                 input_seat_r.error().message());
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_r;
  bool quit = false;
  drm::examples::VtChordTracker vt_chord;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event);
    if (ke == nullptr) {
      return;
    }
    if (vt_chord.observe(*ke, seat ? &*seat : nullptr)) {
      return;
    }
    if (vt_chord.is_quit_key(*ke)) {
      quit = true;
    }
  });

  // Session pause/resume bookkeeping. Both flags are touched from the
  // main thread only — libseat callbacks fire from inside
  // seat->dispatch(), which runs in the main loop. Defer the actual
  // device-fd swap until the loop's next iteration so we don't tear
  // down PageFlip mid-dispatch.
  bool session_paused = false;
  int pending_resume_fd = -1;
  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      flip_pending = false;
      scene->on_session_paused();
      (void)input_seat.suspend();
    });
    seat->set_resume_callback([&](std::string_view path, int new_fd) {
      if (path.substr(0, 9) != "/dev/dri/") {
        return;
      }
      pending_resume_fd = new_fd;
      session_paused = false;
      (void)input_seat.resume();
    });
  }

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit failed: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flip_pending = true;
  drm::println("cluster_sim skeleton: {}x{} — Esc / Q to quit", fb_w, fb_h);

  pollfd pfds[3]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;

  while (!quit) {
    int const timeout = flip_pending ? 16 : -1;
    if (int const ret = poll(pfds, 3, timeout); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      (void)input_seat.dispatch();
    }
    if ((pfds[1].revents & POLLIN) != 0) {
      (void)page_flip.dispatch(0);
    }
    if ((pfds[2].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }

    if (pending_resume_fd != -1) {
      int const new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      ctx.device = drm::Device::from_fd(new_fd);
      pfds[1].fd = dev.fd();
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed");
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed");
        break;
      }
      if (auto r = scene->on_session_resumed(dev); !r) {
        drm::println(stderr, "resume: on_session_resumed: {}", r.error().message());
        break;
      }
      page_flip = drm::PageFlip(dev);
      page_flip.set_handler(
          [&](std::uint32_t, std::uint64_t, std::uint64_t) { flip_pending = false; });
      // The bg buffer's CPU mapping was torn down on pause; repaint
      // against the fresh mapping so the post-resume frame matches the
      // pre-pause frame.
      if (auto m = bg_src_raw->map(drm::MapAccess::Write); m) {
        paint_bg_gradient(*m, fb_w, fb_h);
      }
      if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
        drm::println(stderr, "resume commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
    }
  }

  return EXIT_SUCCESS;
}
