// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// signage_player — a three-layer LayerScene demo driven by a TOML
// playlist. Background slides cycle on a timer (GbmBufferSource), a
// static text overlay sits in the lower third (DumbBufferSource), and
// an optional scrolling ticker repaints every frame on a third
// DumbBufferSource. The three layers' update cadences (per-slide,
// once-ever, every-frame) are deliberately spread so the example
// exercises the per-layer dirty surface that Phase 2.2 will eventually
// minimize property/FB writes against.

#include "common/select_device.hpp"
#include "signage_player/overlay_renderer.hpp"
#include "signage_player/playlist.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/mode.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/gbm_buffer_source.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <system_error>
#include <utility>

namespace {

// 32-bpp ARGB fill. `color` is packed 0xAARRGGBB.
void fill_argb(drm::span<std::uint8_t> pixels, std::uint32_t stride_bytes, std::uint32_t width,
               std::uint32_t height, std::uint32_t color) noexcept {
  const std::uint32_t stride_px = stride_bytes / 4U;
  auto* px = reinterpret_cast<std::uint32_t*>(pixels.data());
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row = px + (static_cast<std::size_t>(y) * stride_px);
    std::fill_n(row, width, color);
  }
}

struct Scene {
  std::unique_ptr<drm::scene::LayerScene> scene;
  drm::scene::GbmBufferSource* bg{nullptr};
  drm::scene::DumbBufferSource* overlay{nullptr};
  drm::scene::DumbBufferSource* ticker{nullptr};
  drm::scene::LayerHandle bg_handle;
  drm::scene::LayerHandle overlay_handle;
  drm::scene::LayerHandle ticker_handle;
};

drm::expected<Scene, std::error_code> build_scene(drm::Device& dev, std::uint32_t crtc_id,
                                                  std::uint32_t connector_id,
                                                  const drmModeModeInfo& mode, std::uint32_t fb_w,
                                                  std::uint32_t fb_h, std::uint32_t overlay_w,
                                                  std::uint32_t overlay_h, std::uint32_t ticker_w,
                                                  std::uint32_t ticker_h, bool with_ticker) {
  auto bg_src = drm::scene::GbmBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src) {
    return drm::unexpected<std::error_code>(bg_src.error());
  }
  auto overlay_src =
      drm::scene::DumbBufferSource::create(dev, overlay_w, overlay_h, DRM_FORMAT_ARGB8888);
  if (!overlay_src) {
    return drm::unexpected<std::error_code>(overlay_src.error());
  }

  std::unique_ptr<drm::scene::DumbBufferSource> ticker_src_owner;
  if (with_ticker) {
    auto ticker_src =
        drm::scene::DumbBufferSource::create(dev, ticker_w, ticker_h, DRM_FORMAT_ARGB8888);
    if (!ticker_src) {
      return drm::unexpected<std::error_code>(ticker_src.error());
    }
    ticker_src_owner = std::move(*ticker_src);
  }

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = crtc_id;
  cfg.connector_id = connector_id;
  cfg.mode = mode;
  auto scene_res = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_res) {
    return drm::unexpected<std::error_code>(scene_res.error());
  }

  Scene out;
  out.scene = std::move(*scene_res);
  out.bg = bg_src->get();
  out.overlay = overlay_src->get();
  out.ticker = ticker_src_owner.get();

  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_src);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.content_type = drm::planes::ContentType::Generic;
  auto bg_h = out.scene->add_layer(std::move(bg_desc));
  if (!bg_h) {
    return drm::unexpected<std::error_code>(bg_h.error());
  }
  out.bg_handle = *bg_h;

  // Overlay centered horizontally, anchored near the bottom third.
  const std::int32_t ox =
      (fb_w > overlay_w) ? static_cast<std::int32_t>((fb_w - overlay_w) / 2U) : 0;
  const std::int32_t oy = (fb_h > overlay_h) ? static_cast<std::int32_t>(fb_h * 2U / 3U) : 0;
  drm::scene::LayerDesc ov_desc;
  ov_desc.source = std::move(*overlay_src);
  ov_desc.display.src_rect = drm::scene::Rect{0, 0, overlay_w, overlay_h};
  ov_desc.display.dst_rect = drm::scene::Rect{ox, oy, overlay_w, overlay_h};
  ov_desc.display.zpos = 4;
  ov_desc.content_type = drm::planes::ContentType::UI;
  auto ov_h = out.scene->add_layer(std::move(ov_desc));
  if (!ov_h) {
    return drm::unexpected<std::error_code>(ov_h.error());
  }
  out.overlay_handle = *ov_h;

  if (with_ticker) {
    // Ticker hugs the bottom of the screen, full width. zpos sits above
    // PRIMARY (amdgpu pins PRIMARY at zpos=2, so anything <=2 here lets
    // the kernel hide the ticker behind the bg) and below the centered
    // overlay; if a slide ever wants the ticker on top of the overlay,
    // raise this above ov_desc.display.zpos.
    const std::int32_t ty = (fb_h > ticker_h) ? static_cast<std::int32_t>(fb_h - ticker_h) : 0;
    drm::scene::LayerDesc tk_desc;
    tk_desc.source = std::move(ticker_src_owner);
    tk_desc.display.src_rect = drm::scene::Rect{0, 0, ticker_w, ticker_h};
    tk_desc.display.dst_rect = drm::scene::Rect{0, ty, ticker_w, ticker_h};
    tk_desc.display.zpos = 3;
    tk_desc.content_type = drm::planes::ContentType::UI;
    auto tk_h = out.scene->add_layer(std::move(tk_desc));
    if (!tk_h) {
      return drm::unexpected<std::error_code>(tk_h.error());
    }
    out.ticker_handle = *tk_h;
  }

  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const char* toml_path = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--playlist") == 0 && i + 1 < argc) {
      toml_path = argv[++i];
    }
  }
  if (toml_path == nullptr) {
    drm::println(stderr, "usage: signage_player --playlist <path.toml> [device]");
    return EXIT_FAILURE;
  }

  auto playlist = signage::Playlist::load(toml_path);
  if (!playlist) {
    drm::println(stderr, "playlist load failed: {}", playlist.error().message());
    return EXIT_FAILURE;
  }
  drm::println("Loaded {} slide(s), overlay: {}, ticker: {}", playlist->slides().size(),
               playlist->overlay().has_value() ? "yes" : "no",
               playlist->ticker().has_value() ? "yes" : "no");

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

  const std::uint32_t overlay_w = std::min<std::uint32_t>(fb_w, 640U);
  const std::uint32_t overlay_h = std::min<std::uint32_t>(fb_h, 96U);
  const bool has_ticker = playlist->ticker().has_value();
  const std::uint32_t ticker_h = has_ticker ? std::min<std::uint32_t>(fb_h, 56U) : 0U;
  const std::uint32_t ticker_w = has_ticker ? fb_w : 0U;

  auto scene_built = build_scene(dev, crtc_id, connector_id, mode.drm_mode, fb_w, fb_h, overlay_w,
                                 overlay_h, ticker_w, ticker_h, has_ticker);
  if (!scene_built) {
    drm::println(stderr, "scene build failed: {}", scene_built.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(scene_built->scene);
  auto* bg = scene_built->bg;
  auto* overlay = scene_built->overlay;
  auto* ticker = scene_built->ticker;

  // Overlay paint is constant — the static-text layer is what makes
  // dirty-tracking interesting later (it never needs a property re-write
  // after the first commit). Computed once here, repeated verbatim on
  // libseat resume.
  signage::OverlayPaint overlay_paint;
  overlay_paint.width = overlay_w;
  overlay_paint.height = overlay_h;
  overlay_paint.stride_bytes = overlay->stride();
  if (const auto& od = playlist->overlay(); od.has_value()) {
    overlay_paint.fg_argb = od->fg_color;
    overlay_paint.bg_argb = od->bg_color;
    overlay_paint.font_size = od->font_size;
    overlay_paint.text = od->text;
  }
  signage::paint_overlay(overlay->pixels(), overlay_paint);

  // Ticker paint is per-frame — the scroll offset advances with elapsed
  // wall time, which makes this layer the dirty-every-frame counterpart
  // to the static overlay above.
  signage::TickerPaint ticker_paint;
  if (has_ticker) {
    const auto& td = *playlist->ticker();
    ticker_paint.width = ticker_w;
    ticker_paint.height = ticker_h;
    ticker_paint.stride_bytes = ticker->stride();
    ticker_paint.fg_argb = td.fg_color;
    ticker_paint.bg_argb = td.bg_color;
    ticker_paint.font_size = td.font_size;
    ticker_paint.text = td.text;
  }
  const auto ticker_started = std::chrono::steady_clock::now();
  auto repaint_ticker = [&](std::chrono::steady_clock::time_point now) {
    if (!has_ticker) {
      return;
    }
    const double elapsed_s = std::chrono::duration<double>(now - ticker_started).count();
    ticker_paint.scroll_offset_px =
        elapsed_s * static_cast<double>(playlist->ticker()->pixels_per_second);
    signage::paint_ticker(ticker->pixels(), ticker_paint);
  };
  repaint_ticker(ticker_started);

  // Paint the first slide into the background before the initial commit.
  auto paint_slide = [&](std::size_t idx) {
    const auto& slide = playlist->slides()[idx];
    const std::uint32_t color =
        (slide.kind == signage::SlideKind::Color) ? slide.color : 0xFF202020U;
    fill_argb(bg->pixels(), bg->stride(), fb_w, fb_h, color);
  };
  paint_slide(0);

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
  bool quit = false;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event)) {
      if (ke->key == KEY_ESC && ke->pressed) {
        quit = true;
      }
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
  drm::println("Running — Escape to quit.");

  pollfd pfds[3]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;

  using clock = std::chrono::steady_clock;
  auto slide_started = clock::now();
  std::size_t slide_idx = 0;

  while (!quit) {
    // When paused we block indefinitely so the CPU is idle until the
    // session resumes or a libinput event wakes us. Otherwise poll
    // with a short timeout so slide timing stays responsive even when
    // no input/seat events arrive.
    int timeout = 0;
    if (session_paused) {
      timeout = -1;
    } else if (flip_pending) {
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
      // Re-paint all three layers against the fresh mappings.
      paint_slide(slide_idx);
      overlay_paint.stride_bytes = overlay->stride();
      signage::paint_overlay(overlay->pixels(), overlay_paint);
      if (has_ticker) {
        ticker_paint.stride_bytes = ticker->stride();
        repaint_ticker(clock::now());
      }
    }

    if (flip_pending || session_paused) {
      continue;
    }

    const auto now = clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - slide_started).count();
    const auto& current = playlist->slides()[slide_idx];
    if (static_cast<std::uint32_t>(elapsed_ms) >= current.duration_ms) {
      slide_idx = (slide_idx + 1) % playlist->slides().size();
      slide_started = now;
      paint_slide(slide_idx);
    }

    repaint_ticker(now);

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
  }

  return EXIT_SUCCESS;
}
