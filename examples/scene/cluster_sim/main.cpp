// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cluster_sim — automotive instrument-cluster showcase. Currently:
// a Blend2D-painted radial-gradient backdrop plus animated speedometer
// and tachometer dial layers. Center info, warning indicators, and an
// optional V4l2DecoderSource rear-view layer land in follow-ups.
//
// Layer stack (bottom up):
//   * Bg: full-screen XRGB8888 dumb buffer, painted once at startup
//     (radial gradient on a dark-blue backdrop).
//   * Speedometer dial: ARGB8888 dumb buffer, repainted each frame
//     with an animated needle. Positioned in the screen's left third.
//   * Tachometer dial: same shape, screen's right third, slightly
//     out-of-phase animation so the two dials don't sweep in lockstep.
//
// The dials demonstrate per-frame Blend2D paint into a dumb buffer
// the LayerScene scans out of directly. There's no double-buffering
// here -- the buffer is mapped, painted, unmapped, and committed each
// frame; brief tearing on the needle is acceptable for an idle demo
// and would be addressed by alternating two DumbBufferSources per
// dial if a real cluster needed tear-free animation.
//
// Key bindings:
//   Esc / q / Ctrl-C — quit.
//   Ctrl+Alt+F<n>    — VT switch (forwarded to libseat).

#include "../../common/open_output.hpp"
#include "../../common/vt_switch.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
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

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
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
// settle on); edges fade darker so the dial layers have somewhere to
// read against.
constexpr std::uint32_t k_bg_center_argb = 0xFF101626U;
constexpr std::uint32_t k_bg_edge_argb = 0xFF02040AU;

// Dial palette. Face is darker than the bg center so the dial reads
// as separate from the backdrop; the rim is a neutral metallic gray.
constexpr std::uint32_t k_dial_rim_argb = 0xFF606870U;
constexpr std::uint32_t k_dial_face_argb = 0xFF080C18U;
constexpr std::uint32_t k_dial_tick_argb = 0xFFC0C8D0U;
constexpr std::uint32_t k_dial_hub_argb = 0xFF1A1F2CU;
constexpr std::uint32_t k_speedo_needle_argb = 0xFFFF3B30U;  // red
constexpr std::uint32_t k_tach_needle_argb = 0xFFFFB300U;    // amber

// Idle animation periods (seconds). Out of phase so the two dials
// don't sweep in lockstep -- a real cluster's dials are decorrelated
// (speed and RPM track different physical signals).
constexpr double k_speedo_period_s = 6.0;
constexpr double k_tach_period_s = 4.0;

// Conventional automotive cluster sweep range: 270° starting from
// the bottom-left and going clockwise to the bottom-right. In
// screen-with-Y-down coordinates (which match Blend2D), the bottom-
// left direction is 3π/4 and a clockwise sweep is positive.
constexpr double k_pi = 3.14159265358979323846;
constexpr double k_dial_start_angle = 3.0 * k_pi / 4.0;
constexpr double k_dial_sweep_angle = 3.0 * k_pi / 2.0;
constexpr int k_dial_major_ticks = 12;

// Convert an idle-animation phase in [0, 1] to a needle-position
// norm in [0, 1] via a (1 - cos)/2 sweep. Smooth at the endpoints,
// peaks in the middle, mirrors back -- the cosine sweep gives a
// slow-fast-slow visual that reads more "dial easing" than a
// triangle wave.
[[nodiscard]] double dial_norm_from_phase(double phase01) noexcept {
  return 0.5 * (1.0 - std::cos(2.0 * k_pi * phase01));
}

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

// Paint a dial face + animated needle into an ARGB8888 dumb buffer.
// `norm` in [0, 1] maps onto the dial's 270° sweep range; `needle_argb`
// distinguishes speedo (red) from tach (amber). Painting clears the
// buffer to transparent first so the dial's circular face renders
// against the bg layer beneath.
void paint_dial(drm::BufferMapping& mapping, std::uint32_t size, double norm,
                std::uint32_t needle_argb) noexcept {
  if (size == 0U) {
    return;
  }
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(size) * stride) {
    return;
  }

  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(size), static_cast<int>(size), BL_FORMAT_PRGB32,
                              pixels.data(), static_cast<intptr_t>(stride), BL_DATA_ACCESS_RW,
                              nullptr, nullptr) != BL_SUCCESS) {
    return;
  }

  BLContext ctx(canvas);
  // Clear to transparent first so the dial reads as a circular cut-out
  // against the bg layer underneath. Subsequent paint ops use SRC_OVER
  // for proper alpha blending of the rim, face, ticks, and needle.
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0x00000000U));
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  double const cx = static_cast<double>(size) / 2.0;
  double const cy = static_cast<double>(size) / 2.0;
  double const r_outer = static_cast<double>(size) * 0.48;
  double const r_inner = static_cast<double>(size) * 0.42;
  double const r_tick_outer = static_cast<double>(size) * 0.46;
  double const r_tick_inner = static_cast<double>(size) * 0.38;
  double const r_needle = static_cast<double>(size) * 0.40;
  double const r_hub = static_cast<double>(size) * 0.06;

  // Rim (filled circle in metallic gray) and face (slightly inset
  // dark fill) — fill_circle layers correctly because of SRC_OVER.
  ctx.fill_circle(BLCircle(cx, cy, r_outer), BLRgba32(k_dial_rim_argb));
  ctx.fill_circle(BLCircle(cx, cy, r_inner), BLRgba32(k_dial_face_argb));

  // Major ticks (12 + 1 to close the sweep range) — light gray lines
  // running radially outward across the rim. Using set_stroke_width
  // ahead of stroke_line because Blend2D's per-call stroke API doesn't
  // take a width inline.
  double const a_norm = std::clamp(norm, 0.0, 1.0);
  ctx.set_stroke_width(3.0);
  for (int i = 0; i <= k_dial_major_ticks; ++i) {
    double const t = static_cast<double>(i) / static_cast<double>(k_dial_major_ticks);
    double const a = k_dial_start_angle + (t * k_dial_sweep_angle);
    double const x1 = cx + (r_tick_inner * std::cos(a));
    double const y1 = cy + (r_tick_inner * std::sin(a));
    double const x2 = cx + (r_tick_outer * std::cos(a));
    double const y2 = cy + (r_tick_outer * std::sin(a));
    ctx.stroke_line(BLPoint(x1, y1), BLPoint(x2, y2), BLRgba32(k_dial_tick_argb));
  }

  // Needle: thick rounded line from center to the tip computed from
  // the normalized position. Stroke cap is round so the tip reads as
  // a smooth point rather than a chopped rectangle.
  double const needle_a = k_dial_start_angle + (a_norm * k_dial_sweep_angle);
  double const nx = cx + (r_needle * std::cos(needle_a));
  double const ny = cy + (r_needle * std::sin(needle_a));
  ctx.set_stroke_width(5.0);
  ctx.set_stroke_caps(BL_STROKE_CAP_ROUND);
  ctx.stroke_line(BLPoint(cx, cy), BLPoint(nx, ny), BLRgba32(needle_argb));

  // Hub cap covering the needle's pivot point.
  ctx.fill_circle(BLCircle(cx, cy, r_hub), BLRgba32(k_dial_hub_argb));

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

  // Dial sizing: 4/9 of screen height capped at 400 px gives ~400 on
  // 1080p, 320 on 720p, scales down sensibly for smaller outputs.
  // Centered vertically; speedo at 1/4-screen, tach at 3/4-screen.
  std::uint32_t const dial_size = std::min<std::uint32_t>(fb_h * 4U / 9U, 400U);
  auto const dial_y = static_cast<std::int32_t>((fb_h - dial_size) / 2U);
  auto const speedo_x =
      static_cast<std::int32_t>(fb_w / 4U) - static_cast<std::int32_t>(dial_size / 2U);
  auto const tach_x =
      static_cast<std::int32_t>((fb_w * 3U) / 4U) - static_cast<std::int32_t>(dial_size / 2U);

  auto make_dial_layer =
      [&](std::int32_t x,
          std::int32_t y) -> drm::expected<drm::scene::DumbBufferSource*, std::error_code> {
    auto src_r =
        drm::scene::DumbBufferSource::create(dev, dial_size, dial_size, DRM_FORMAT_ARGB8888);
    if (!src_r) {
      return drm::unexpected<std::error_code>(src_r.error());
    }
    auto src = std::move(*src_r);
    auto* raw = src.get();
    drm::scene::LayerDesc desc;
    desc.source = std::move(src);
    desc.display.src_rect = drm::scene::Rect{0, 0, dial_size, dial_size};
    desc.display.dst_rect = drm::scene::Rect{x, y, dial_size, dial_size};
    // amdgpu pins PRIMARY at zpos=2; the dials need to sit above the
    // bg, so anchor them at zpos>=3 explicitly to avoid silent
    // collision with the primary plane.
    desc.display.zpos = 3;
    if (auto h = scene->add_layer(std::move(desc)); !h) {
      return drm::unexpected<std::error_code>(h.error());
    }
    return raw;
  };

  auto speedo_r = make_dial_layer(speedo_x, dial_y);
  if (!speedo_r) {
    drm::println(stderr, "add_layer (speedo): {}", speedo_r.error().message());
    return EXIT_FAILURE;
  }
  auto tach_r = make_dial_layer(tach_x, dial_y);
  if (!tach_r) {
    drm::println(stderr, "add_layer (tach): {}", tach_r.error().message());
    return EXIT_FAILURE;
  }
  auto* speedo_src_raw = *speedo_r;
  auto* tach_src_raw = *tach_r;

  // Paint the dials at norm=0 so the first commit has valid pixel
  // content for both layers.
  auto repaint_dials = [&](double speedo_norm, double tach_norm) {
    if (auto m = speedo_src_raw->map(drm::MapAccess::Write); m) {
      paint_dial(*m, dial_size, speedo_norm, k_speedo_needle_argb);
    }
    if (auto m = tach_src_raw->map(drm::MapAccess::Write); m) {
      paint_dial(*m, dial_size, tach_norm, k_tach_needle_argb);
    }
  };
  repaint_dials(0.0, 0.0);

  bool flip_pending = false;
  // need_repaint drives the per-frame dial-paint cycle. Set true on
  // every page-flip-event landing so the next loop iteration repaints
  // and commits, giving us a flip-driven ~vsync-locked animation
  // cadence without a wall-clock timer.
  bool need_repaint = false;
  drm::PageFlip page_flip(dev);
  page_flip.set_handler([&](std::uint32_t /*c*/, std::uint64_t /*s*/, std::uint64_t /*t*/) {
    flip_pending = false;
    need_repaint = true;
  });

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
  auto const start_time = std::chrono::steady_clock::now();
  drm::println("cluster_sim: {}x{} — Esc / Q to quit", fb_w, fb_h);

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
      page_flip.set_handler([&](std::uint32_t, std::uint64_t, std::uint64_t) {
        flip_pending = false;
        need_repaint = true;
      });
      // Buffer mappings were torn down on pause; repaint the bg + both
      // dials against the fresh mappings before the resume commit so
      // the post-resume frame is intact rather than stale-or-blank.
      if (auto m = bg_src_raw->map(drm::MapAccess::Write); m) {
        paint_bg_gradient(*m, fb_w, fb_h);
      }
      double const elapsed_resume =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
      double const speedo_resume = dial_norm_from_phase(elapsed_resume / k_speedo_period_s);
      double const tach_resume = dial_norm_from_phase(elapsed_resume / k_tach_period_s);
      repaint_dials(speedo_resume, tach_resume);
      if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
        drm::println(stderr, "resume commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
    }

    // Per-frame paint cycle: only when the prior flip has landed and
    // the session is live. Repaint both dials against their current
    // animation phase, then commit -- the kernel queues the flip for
    // the next vblank, the page-flip handler clears flip_pending and
    // sets need_repaint again, and the loop ticks at scanout cadence.
    if (need_repaint && !flip_pending && !session_paused) {
      need_repaint = false;
      double const elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
      double const speedo_norm = dial_norm_from_phase(elapsed / k_speedo_period_s);
      double const tach_norm = dial_norm_from_phase(elapsed / k_tach_period_s);
      repaint_dials(speedo_norm, tach_norm);
      auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
      if (!r) {
        if (r.error() == std::errc::permission_denied) {
          // Master got revoked between the flip event and the next
          // commit (libseat hasn't fired pause_cb yet). Treat as a
          // soft pause and let the resume path put us back together.
          session_paused = true;
          flip_pending = false;
          continue;
        }
        drm::println(stderr, "commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
    }
  }

  return EXIT_SUCCESS;
}
