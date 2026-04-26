// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// signage_player — a five-layer LayerScene demo driven by a TOML
// playlist. Background slides cycle on a timer (GbmBufferSource), a
// static text overlay sits in the lower third (DumbBufferSource), an
// optional scrolling ticker repaints every frame on a third
// DumbBufferSource, an optional clock badge in the top-right corner
// repaints only when the formatted time string changes (once per
// minute with the default "%H:%M"), and an optional logo bug in the
// top-left corner is painted once from a PNG. The five layers' update
// cadences (per-slide, once-ever, every-frame, once-per-minute,
// never-after-load) are deliberately spread so the example exercises
// the per-layer dirty surface that Phase 2.2 will eventually minimize
// property/FB writes against.

#include "common/open_output.hpp"
#include "signage_player/overlay_renderer.hpp"
#include "signage_player/playlist.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/hotplug_monitor.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/mode.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/gbm_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <variant>

namespace {

// 32-bpp ARGB fill. `color` is packed 0xAARRGGBB. Validates the span
// before writing so a misshapen mapping (e.g. a stride/height that no
// longer matches what was allocated, or a span shortened by a future
// resume bug) can't punch past the buffer end.
void fill_argb(drm::span<std::uint8_t> pixels, std::uint32_t stride_bytes, std::uint32_t width,
               std::uint32_t height, std::uint32_t color) noexcept {
  if (width == 0U || height == 0U || stride_bytes < (width * 4U)) {
    return;
  }
  if (pixels.size() < static_cast<std::size_t>(height) * stride_bytes) {
    return;
  }
  const std::uint32_t stride_px = stride_bytes / 4U;
  auto* px = reinterpret_cast<std::uint32_t*>(pixels.data());
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row = px + (static_cast<std::size_t>(y) * stride_px);
    std::fill_n(row, width, color);
  }
}

// Resolved (connector, crtc, mode) tuple — produced by find_active_target,
// consumed by --hotplug-follow's rebind path.
struct ActiveTarget {
  std::uint32_t connector_id{0};
  std::uint32_t crtc_id{0};
  drmModeModeInfo mode{};
};

// First connected connector with at least one mode and a usable
// encoder + CRTC. Mirrors the startup-time selection but split into a
// reusable helper so --hotplug-follow can re-run it on each event.
std::optional<ActiveTarget> find_active_target(int drm_fd) {
  const auto res = drm::get_resources(drm_fd);
  if (!res) {
    return std::nullopt;
  }
  for (int i = 0; i < res->count_connectors; ++i) {
    auto conn = drm::get_connector(drm_fd, res->connectors[i]);
    if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0 ||
        conn->encoder_id == 0) {
      continue;
    }
    auto enc = drm::get_encoder(drm_fd, conn->encoder_id);
    if (!enc || enc->crtc_id == 0) {
      continue;
    }
    const auto mode = drm::select_preferred_mode(
        drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes));
    if (!mode) {
      continue;
    }
    ActiveTarget out;
    out.connector_id = conn->connector_id;
    out.crtc_id = enc->crtc_id;
    out.mode = mode->drm_mode;
    return out;
  }
  return std::nullopt;
}

bool same_mode(const drmModeModeInfo& a, const drmModeModeInfo& b) {
  return a.hdisplay == b.hdisplay && a.vdisplay == b.vdisplay && a.clock == b.clock &&
         a.vrefresh == b.vrefresh;
}

struct Scene {
  std::unique_ptr<drm::scene::LayerScene> scene;
  drm::scene::GbmBufferSource* bg{nullptr};
  drm::scene::DumbBufferSource* overlay{nullptr};
  drm::scene::DumbBufferSource* ticker{nullptr};
  drm::scene::DumbBufferSource* clock{nullptr};
  drm::scene::DumbBufferSource* logo{nullptr};
  drm::scene::LayerHandle bg_handle;
  drm::scene::LayerHandle overlay_handle;
  drm::scene::LayerHandle ticker_handle;
  drm::scene::LayerHandle clock_handle;
  drm::scene::LayerHandle logo_handle;
};

struct SceneDims {
  std::uint32_t fb_w;
  std::uint32_t fb_h;
  std::uint32_t overlay_w;
  std::uint32_t overlay_h;
  std::uint32_t ticker_w;
  std::uint32_t ticker_h;
  std::uint32_t clock_w;
  std::uint32_t clock_h;
  std::uint32_t logo_w;
  std::uint32_t logo_h;
  bool with_ticker;
  bool with_clock;
  bool with_logo;
};

drm::expected<Scene, std::error_code> build_scene(drm::Device& dev, std::uint32_t crtc_id,
                                                  std::uint32_t connector_id,
                                                  const drmModeModeInfo& mode,
                                                  const SceneDims& dims) {
  const auto fb_w = dims.fb_w;
  const auto fb_h = dims.fb_h;
  const auto overlay_w = dims.overlay_w;
  const auto overlay_h = dims.overlay_h;
  const auto ticker_w = dims.ticker_w;
  const auto ticker_h = dims.ticker_h;
  const auto clock_w = dims.clock_w;
  const auto clock_h = dims.clock_h;
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
  if (dims.with_ticker) {
    auto ticker_src =
        drm::scene::DumbBufferSource::create(dev, ticker_w, ticker_h, DRM_FORMAT_ARGB8888);
    if (!ticker_src) {
      return drm::unexpected<std::error_code>(ticker_src.error());
    }
    ticker_src_owner = std::move(*ticker_src);
  }

  std::unique_ptr<drm::scene::DumbBufferSource> clock_src_owner;
  if (dims.with_clock) {
    auto clock_src =
        drm::scene::DumbBufferSource::create(dev, clock_w, clock_h, DRM_FORMAT_ARGB8888);
    if (!clock_src) {
      return drm::unexpected<std::error_code>(clock_src.error());
    }
    clock_src_owner = std::move(*clock_src);
  }

  std::unique_ptr<drm::scene::DumbBufferSource> logo_src_owner;
  if (dims.with_logo) {
    auto logo_src =
        drm::scene::DumbBufferSource::create(dev, dims.logo_w, dims.logo_h, DRM_FORMAT_ARGB8888);
    if (!logo_src) {
      return drm::unexpected<std::error_code>(logo_src.error());
    }
    logo_src_owner = std::move(*logo_src);
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
  out.clock = clock_src_owner.get();
  out.logo = logo_src_owner.get();

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

  if (dims.with_ticker) {
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

  if (dims.with_clock) {
    // The clock badge sits in the top-right corner with a small margin. zpos
    // is above ticker/overlay (overlay=4, ticker=3, bg=2), so the
    // timestamp is never occluded by another UI; the logo bug below sits
    // one rung higher still.
    constexpr std::uint32_t k_clock_margin = 16U;
    const std::int32_t cx = (fb_w > clock_w + k_clock_margin)
                                ? static_cast<std::int32_t>(fb_w - clock_w - k_clock_margin)
                                : 0;
    constexpr auto cy = static_cast<std::int32_t>(k_clock_margin);
    drm::scene::LayerDesc ck_desc;
    ck_desc.source = std::move(clock_src_owner);
    ck_desc.display.src_rect = drm::scene::Rect{0, 0, clock_w, clock_h};
    ck_desc.display.dst_rect = drm::scene::Rect{cx, cy, clock_w, clock_h};
    ck_desc.display.zpos = 5;
    ck_desc.content_type = drm::planes::ContentType::UI;
    auto ck_h = out.scene->add_layer(std::move(ck_desc));
    if (!ck_h) {
      return drm::unexpected<std::error_code>(ck_h.error());
    }
    out.clock_handle = *ck_h;
  }

  if (dims.with_logo) {
    // Logo bug in the top-left corner with the same margin as the
    // clock. zpos sits above every other UI layer (overlay=4,
    // ticker=3, clock=5), so brand chrome is never occluded.
    constexpr std::uint32_t k_logo_margin = 16U;
    constexpr auto lx = static_cast<std::int32_t>(k_logo_margin);
    constexpr auto ly = static_cast<std::int32_t>(k_logo_margin);
    drm::scene::LayerDesc lg_desc;
    lg_desc.source = std::move(logo_src_owner);
    lg_desc.display.src_rect = drm::scene::Rect{0, 0, dims.logo_w, dims.logo_h};
    lg_desc.display.dst_rect = drm::scene::Rect{lx, ly, dims.logo_w, dims.logo_h};
    lg_desc.display.zpos = 6;
    lg_desc.content_type = drm::planes::ContentType::UI;
    auto lg_h = out.scene->add_layer(std::move(lg_desc));
    if (!lg_h) {
      return drm::unexpected<std::error_code>(lg_h.error());
    }
    out.logo_handle = *lg_h;
  }

  return out;
}

// strftime against the wall-clock now. Returns empty on overflow / bad
// format. 64 chars is comfortable for any sane time string; if a user
// supplies a format that exceeds it, we'd rather render nothing than
// truncate silent mid-string.
std::string format_now(const std::string& fmt) noexcept {
  std::array<char, 64> buf{};
  const auto t = std::time(nullptr);
  std::tm tm_local{};
  if (localtime_r(&t, &tm_local) == nullptr) {
    return {};
  }
  const std::size_t n = std::strftime(buf.data(), buf.size(), fmt.c_str(), &tm_local);
  if (n == 0U) {
    return {};
  }
  return {buf.data(), n};
}

}  // namespace

int main(int argc, char** argv) {
  const char* toml_path = nullptr;
  bool hotplug_follow = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--playlist") == 0 && i + 1 < argc) {
      toml_path = argv[++i];
    } else if (std::strcmp(argv[i], "--hotplug-follow") == 0) {
      hotplug_follow = true;
    }
  }
  if (toml_path == nullptr) {
    drm::println(stderr,
                 "usage: signage_player --playlist <path.toml> [--hotplug-follow] [device]");
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

  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  auto& seat = output->seat;
  const std::uint32_t crtc_id = output->crtc_id;
  const std::uint32_t connector_id = output->connector_id;
  const drmModeModeInfo mode = output->mode;
  const std::uint32_t fb_w = mode.hdisplay;
  const std::uint32_t fb_h = mode.vdisplay;
  drm::println("Mode: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h, mode.vrefresh,
               connector_id, crtc_id);

  bool session_paused = false;
  bool flip_pending = false;
  int pending_resume_fd = -1;

  // Layer count is whatever the playlist asks for. LayerScene's Phase 2.3
  // composition fallback CPU-blends any layers the allocator can't fit
  // onto a hardware plane onto a shared canvas, so a 3-layer playlist
  // on a 2-OVERLAY GPU degrades to one composited canvas + bg without
  // the example having to shed optional layers up front.
  const bool has_ticker = playlist->ticker().has_value();
  const bool has_clock = playlist->clock().has_value();
  const bool has_logo = playlist->logo().has_value();

  const std::uint32_t overlay_w = std::min<std::uint32_t>(fb_w, 640U);
  const std::uint32_t overlay_h = std::min<std::uint32_t>(fb_h, 96U);
  const std::uint32_t ticker_h = has_ticker ? std::min<std::uint32_t>(fb_h, 56U) : 0U;
  const std::uint32_t ticker_w = has_ticker ? fb_w : 0U;
  // Size the clock buffer from font_size so a larger format ("%H:%M:%S")
  // or a bumped font still fits — width holds ~5 ems (enough for the
  // longest strftime we expect from a sensible playlist), height is 2×
  // ems so ascent+descent has breathing room.
  const std::uint32_t clock_font_size = has_clock ? playlist->clock()->font_size : 0U;
  const std::uint32_t clock_w =
      has_clock ? std::min<std::uint32_t>(fb_w, std::max<std::uint32_t>(192U, clock_font_size * 5U))
                : 0U;
  const std::uint32_t clock_h =
      has_clock ? std::min<std::uint32_t>(fb_h, std::max<std::uint32_t>(72U, clock_font_size * 2U))
                : 0U;
  // Logo dims come straight from the playlist — clamped to the
  // framebuffer in case someone configures something silly.
  const std::uint32_t logo_w =
      has_logo ? std::min<std::uint32_t>(fb_w, playlist->logo()->width) : 0U;
  const std::uint32_t logo_h =
      has_logo ? std::min<std::uint32_t>(fb_h, playlist->logo()->height) : 0U;

  SceneDims dims{};
  dims.fb_w = fb_w;
  dims.fb_h = fb_h;
  dims.overlay_w = overlay_w;
  dims.overlay_h = overlay_h;
  dims.ticker_w = ticker_w;
  dims.ticker_h = ticker_h;
  dims.clock_w = clock_w;
  dims.clock_h = clock_h;
  dims.logo_w = logo_w;
  dims.logo_h = logo_h;
  dims.with_ticker = has_ticker;
  dims.with_clock = has_clock;
  dims.with_logo = has_logo;

  auto scene_built = build_scene(dev, crtc_id, connector_id, mode, dims);
  if (!scene_built) {
    drm::println(stderr, "scene build failed: {}", scene_built.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(scene_built->scene);
  auto* bg = scene_built->bg;
  auto* overlay = scene_built->overlay;
  auto* ticker = scene_built->ticker;
  auto* clock_src = scene_built->clock;
  auto* logo_src = scene_built->logo;

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

  // Clock paint is dirty-once-per-minute — the renderer is invoked only
  // when the formatted time string changes, which is the once-per-minute
  // counterpart to the static overlay and dirty-every-frame ticker.
  signage::ClockPaint clock_paint;
  std::string clock_format;
  std::string last_clock_text;
  if (has_clock) {
    const auto& cd = *playlist->clock();
    clock_paint.width = clock_w;
    clock_paint.height = clock_h;
    clock_paint.stride_bytes = clock_src->stride();
    clock_paint.fg_argb = cd.fg_color;
    clock_paint.bg_argb = cd.bg_color;
    clock_paint.font_size = cd.font_size;
    clock_format = cd.format;
  }
  auto repaint_clock_if_changed = [&]() {
    if (!has_clock) {
      return;
    }
    auto formatted = format_now(clock_format);
    if (formatted == last_clock_text) {
      return;
    }
    last_clock_text = std::move(formatted);
    clock_paint.text = last_clock_text;
    signage::paint_clock(clock_src->pixels(), clock_paint);
  };
  repaint_clock_if_changed();

  // Logo paint is dirty-once (and once-after-resume since the dumb
  // buffer is remapped). Loaded eagerly so the very first commit
  // already contains the brand bug.
  signage::LogoPaint logo_paint;
  if (has_logo) {
    const auto& ld = *playlist->logo();
    logo_paint.width = logo_w;
    logo_paint.height = logo_h;
    logo_paint.stride_bytes = logo_src->stride();
    logo_paint.path = ld.path;
    logo_paint.fallback_argb = ld.fallback_color;
    signage::paint_logo(logo_src->pixels(), logo_paint);
  }

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

  // Optional hotplug-follow plumbing. The library's HotplugMonitor
  // exposes a netlink fd we add to the poll set; on every event we
  // rescan for the best (connector, crtc, mode) and call rebind() if
  // the answer changed. Existing layers keep their handles and their
  // backing buffers — we don't resize the bg/overlay/ticker buffers
  // here, so a new mode with different dimensions will letterbox or
  // crop. That's an honest demo of the rebind primitive; a production
  // app would also rebuild layer buffers at the new dimensions.
  std::optional<drm::display::HotplugMonitor> hotplug_monitor;
  drmModeModeInfo current_mode = mode;
  std::uint32_t current_crtc_id = crtc_id;
  std::uint32_t current_connector_id = connector_id;
  if (hotplug_follow) {
    auto m = drm::display::HotplugMonitor::open();
    if (!m) {
      drm::println(stderr, "HotplugMonitor::open: {}", m.error().message());
      return EXIT_FAILURE;
    }
    hotplug_monitor = std::move(*m);
    hotplug_monitor->set_handler([&](const drm::display::HotplugEvent& /*ev*/) {
      const auto fresh = find_active_target(dev.fd());
      if (!fresh) {
        drm::println("[hotplug] no connected connector — staying on the previous binding");
        return;
      }
      if (fresh->connector_id == current_connector_id && fresh->crtc_id == current_crtc_id &&
          same_mode(fresh->mode, current_mode)) {
        return;
      }
      drm::println("[hotplug] rebinding to connector={} crtc={} mode={}x{}@{}Hz",
                   fresh->connector_id, fresh->crtc_id, fresh->mode.hdisplay, fresh->mode.vdisplay,
                   fresh->mode.vrefresh);
      auto report = scene->rebind(fresh->crtc_id, fresh->connector_id, fresh->mode);
      if (!report) {
        drm::println(stderr, "rebind: {}", report.error().message());
        return;
      }
      if (!report->empty()) {
        drm::println("  ({} layer(s) flagged incompatible against the new mode)",
                     report->incompatibilities.size());
      }
      current_connector_id = fresh->connector_id;
      current_crtc_id = fresh->crtc_id;
      current_mode = fresh->mode;
      // Force the next loop iteration to commit so the rebound CRTC
      // gets its modeset + first frame promptly.
      flip_pending = false;
    });
    drm::println("Hotplug-follow enabled.");
  }

  drm::println("Running — Escape to quit.");

  pollfd pfds[4]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;
  pfds[3].fd = hotplug_monitor ? hotplug_monitor->fd() : -1;
  pfds[3].events = POLLIN;

  using clock = std::chrono::steady_clock;
  auto slide_started = clock::now();
  std::size_t slide_idx = 0;

  while (!quit) {
    // When paused, we block indefinitely, so the CPU is idle until the
    // session resumes or a libinput event wakes us. Otherwise, poll
    // with a short timeout, so slide timing stays responsive even when
    // no input/seat events arrive.
    int timeout = 0;
    if (session_paused) {
      timeout = -1;
    } else if (flip_pending) {
      timeout = 16;
    }
    if (const int ret = poll(pfds, 4, timeout); ret < 0) {
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
    if ((pfds[3].revents & POLLIN) != 0 && hotplug_monitor) {
      (void)hotplug_monitor->dispatch();
    }

    if (pending_resume_fd != -1) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      scene->on_session_paused();
      output->device = drm::Device::from_fd(new_fd);
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
      // Re-paint every layer against the fresh mappings.
      paint_slide(slide_idx);
      overlay_paint.stride_bytes = overlay->stride();
      signage::paint_overlay(overlay->pixels(), overlay_paint);
      if (has_ticker) {
        ticker_paint.stride_bytes = ticker->stride();
        repaint_ticker(clock::now());
      }
      if (has_clock) {
        clock_paint.stride_bytes = clock_src->stride();
        // Force the clock to repaint after resume even if the minute
        // hasn't rolled — the underlying buffer was unmapped and is
        // fresh again.
        last_clock_text.clear();
        repaint_clock_if_changed();
      }
      if (has_logo) {
        logo_paint.stride_bytes = logo_src->stride();
        signage::paint_logo(logo_src->pixels(), logo_paint);
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
    repaint_clock_if_changed();

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
