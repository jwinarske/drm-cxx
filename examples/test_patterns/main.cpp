// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// test_patterns — single full-screen LayerScene layer cycling through
// a fixed set of display-engineering reference patterns. Demonstrates
// the paint-on-event workload: the layer is repainted only when the
// user switches patterns, so the example exercises a complementary
// LayerScene cadence to signage_player's mixed per-frame / per-minute
// / once-only updates.
//
// Key bindings:
//   1 .. 8  — select pattern by index
//   n / Space — next pattern (wrap)
//   p       — previous pattern (wrap)
//   Esc / q — quit

#include "common/select_device.hpp"
#include "test_patterns/patterns.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/mode.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <optional>
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <variant>

namespace {

using drm::examples::test_patterns::PaintTarget;
using drm::examples::test_patterns::PatternKind;

// Map a libinput KEY_* code to a pattern selector. Returns nullopt for
// keys the example doesn't bind, so the caller knows whether the press
// was meaningful.
std::optional<PatternKind> key_to_pattern(std::uint32_t key) noexcept {
  switch (key) {
    case KEY_1:
      return PatternKind::SmpteBars;
    case KEY_2:
      return PatternKind::PixelStripes;
    case KEY_3:
      return PatternKind::GrayBars;
    case KEY_4:
      return PatternKind::Checkerboard;
    case KEY_5:
      return PatternKind::GrayGradient;
    case KEY_6:
      return PatternKind::ColorGradient;
    case KEY_7:
      return PatternKind::CrossHatch;
    case KEY_8:
      return PatternKind::HPattern;
    default:
      return std::nullopt;
  }
}

PatternKind next_pattern(PatternKind current) noexcept {
  using drm::examples::test_patterns::k_pattern_count;
  const auto idx = static_cast<std::uint8_t>(current);
  return static_cast<PatternKind>((idx + 1U) % k_pattern_count);
}

PatternKind prev_pattern(PatternKind current) noexcept {
  using drm::examples::test_patterns::k_pattern_count;
  const auto idx = static_cast<std::uint8_t>(current);
  return static_cast<PatternKind>((idx + k_pattern_count - 1U) % k_pattern_count);
}

}  // namespace

int main(int argc, char** argv) {
  const auto device_path = drm::examples::select_device(argc, argv);
  if (!device_path) {
    return EXIT_FAILURE;
  }

  auto seat = drm::session::Seat::open();
  bool session_paused = false;
  bool flip_pending = false;
  int pending_resume_fd = -1;

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

  // Single full-screen XRGB8888 layer. XRGB (not ARGB) — this is the
  // primary plane content with no transparency, matching the signage
  // example's background convention.
  auto bg_src = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src) {
    drm::println(stderr, "DumbBufferSource::create failed: {}", bg_src.error().message());
    return EXIT_FAILURE;
  }
  auto* bg = bg_src->get();

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = crtc_id;
  cfg.connector_id = connector_id;
  cfg.mode = mode.drm_mode;
  auto scene_res = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_res) {
    drm::println(stderr, "LayerScene::create failed: {}", scene_res.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_res);

  drm::scene::LayerDesc desc;
  desc.source = std::move(*bg_src);
  desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  desc.content_type = drm::planes::ContentType::Generic;
  auto bg_h = scene->add_layer(std::move(desc));
  if (!bg_h) {
    drm::println(stderr, "add_layer failed: {}", bg_h.error().message());
    return EXIT_FAILURE;
  }

  // Paint helper closes over the buffer so the input handler can drive
  // pattern switches with a single function call.
  PatternKind current = PatternKind::SmpteBars;
  auto repaint = [&](PatternKind kind) noexcept {
    PaintTarget tgt;
    tgt.pixels = bg->pixels();
    tgt.stride_bytes = bg->stride();
    tgt.width = fb_w;
    tgt.height = fb_h;
    drm::examples::test_patterns::paint(kind, tgt);
  };
  repaint(current);

  drm::PageFlip page_flip(dev);
  page_flip.set_handler(
      [&](std::uint32_t /*c*/, std::uint64_t /*s*/, std::uint64_t /*t*/) { flip_pending = false; });

  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_res = drm::input::Seat::open({}, std::move(libinput_opener));
  if (!input_seat_res) {
    drm::println(stderr, "Failed to open input seat (need root or 'input' group membership)");
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_res;

  // Coalesce rapid keypresses against the page-flip pacing: if a switch
  // arrives while a flip is still pending, remember it and apply on the
  // next idle iteration. Avoids piling commits on top of each other.
  bool quit = false;
  std::optional<PatternKind> pending_kind;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event);
    if (ke == nullptr || !ke->pressed) {
      return;
    }
    if (ke->key == KEY_ESC || ke->key == KEY_Q) {
      quit = true;
      return;
    }
    if (ke->key == KEY_N || ke->key == KEY_SPACE) {
      pending_kind = next_pattern(pending_kind.value_or(current));
      return;
    }
    if (ke->key == KEY_P) {
      pending_kind = prev_pattern(pending_kind.value_or(current));
      return;
    }
    if (auto k = key_to_pattern(ke->key); k.has_value()) {
      pending_kind = k;
    }
  });

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

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit failed: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flip_pending = true;
  drm::println("Running pattern: {} — 1..8 to select, n/p to step, Esc/q to quit.",
               drm::examples::test_patterns::name_of(current));

  pollfd pfds[3]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;

  while (!quit) {
    // Block indefinitely when paused or idle (no pending pattern
    // switch) — the example is event-driven, no slide timer to keep
    // alive. While a flip is in flight, cap the wait at one frame so a
    // late page-flip event doesn't stall the next switch noticeably.
    int timeout = -1;
    if (flip_pending) {
      timeout = 16;
    }
    if (const int ret = poll(pfds, 3, timeout); ret < 0) {
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
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      scene->on_session_paused();
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
      if (auto r = scene->on_session_resumed(dev); !r) {
        drm::println(stderr, "resume: on_session_resumed failed: {}", r.error().message());
        break;
      }
      page_flip = drm::PageFlip(dev);
      page_flip.set_handler(
          [&](std::uint32_t, std::uint64_t, std::uint64_t) { flip_pending = false; });
      // Buffer mapping was torn down on pause; repaint the current
      // pattern against the fresh mmap before the next commit.
      repaint(current);
    }

    if (flip_pending || session_paused) {
      continue;
    }

    if (pending_kind.has_value() && *pending_kind != current) {
      current = *pending_kind;
      pending_kind.reset();
      repaint(current);
      drm::println("Pattern: {}", drm::examples::test_patterns::name_of(current));
      if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
          !r) {
        if (r.error() == std::errc::permission_denied) {
          session_paused = true;
          flip_pending = false;
          continue;
        }
        drm::println(stderr, "commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
    } else {
      // Same kind requested — drop without re-committing.
      pending_kind.reset();
    }
  }

  return EXIT_SUCCESS;
}
