// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// layered_demo — interactive pedagogical example exercising the
// LayerScene mutation API. A static gradient background is always
// present; up to eight coloured tiles can be added, removed, moved,
// re-stacked, and re-shaded at runtime. Each user input results in a
// scene mutation followed by a single commit, so the example doubles
// as a tour of which API call drives which kind of property write.
//
// Selection is tracked locally — number keys both toggle a tile's
// presence and select it, Tab walks the currently-active tiles in
// round-robin order. All mutations apply to the selected tile (when
// active); on an inactive selection the input is dropped silently
// because there is no scene state to mutate.
//
// Key bindings:
//   1 .. 8         — toggle tile N (also selects it)
//   Tab            — cycle selection through the active tiles
//   Arrow keys     — move the selected tile by 32 px
//   z / x          — lower / raise the selected tile's zpos (clamped 3..10)
//   [ / ]          — decrease / increase the selected tile's alpha
//   r              — reset every tile to its starting position / state
//   F1             — print the current scene state + last commit report
//   Esc / q        — quit

#include "common/format_probe.hpp"
#include "common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer.hpp>
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
#include <cstddef>
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

constexpr std::uint32_t k_tile_w = 256U;
constexpr std::uint32_t k_tile_h = 160U;
constexpr std::int32_t k_move_step_px = 32;
constexpr std::int32_t k_zpos_min = 3;
constexpr std::int32_t k_zpos_max = 10;
constexpr std::uint16_t k_alpha_step = 0x2000U;
constexpr std::uint32_t k_tile_count = 8U;

constexpr std::array<std::uint32_t, k_tile_count> k_tile_palette{
    0xFFE57373U, 0xFF81C784U, 0xFF64B5F6U, 0xFFFFD54FU,
    0xFFBA68C8U, 0xFF4DB6ACU, 0xFFFF8A65U, 0xFF7986CBU,
};

struct Tile {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t zpos{k_zpos_min};
  std::uint16_t alpha{0xFFFFU};
  std::uint32_t color{0};
  bool active{false};
  drm::scene::LayerHandle handle{};
};

void fill_argb_rect(drm::span<std::uint8_t> pixels, std::uint32_t stride_bytes, std::uint32_t width,
                    std::uint32_t height, std::uint32_t color_argb) noexcept {
  if (width == 0U || height == 0U || stride_bytes < width * 4U) {
    return;
  }
  if (pixels.size() < static_cast<std::size_t>(height) * stride_bytes) {
    return;
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row = reinterpret_cast<std::uint32_t*>(pixels.data() +
                                                 (static_cast<std::size_t>(y) * stride_bytes));
    for (std::uint32_t x = 0; x < width; ++x) {
      row[x] = color_argb;
    }
  }
}

// Soft horizontal+vertical gradient for the background. Pure XRGB so
// the byte-order check and the "did we reach scanout" check both come
// for free — a wrong-channel write would tint the gradient obviously.
void paint_bg_gradient(drm::BufferMapping& map) noexcept {
  const auto width = map.width();
  const auto height = map.height();
  const auto stride_bytes = map.stride();
  const auto pixels = map.pixels();
  if (width == 0U || height == 0U || stride_bytes < width * 4U) {
    return;
  }
  if (pixels.size() < static_cast<std::size_t>(height) * stride_bytes) {
    return;
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto v = static_cast<std::uint8_t>((y * 96U) / std::max<std::uint32_t>(1U, height - 1U));
    auto* row = reinterpret_cast<std::uint32_t*>(pixels.data() +
                                                 (static_cast<std::size_t>(y) * stride_bytes));
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto h = static_cast<std::uint8_t>((x * 64U) / std::max<std::uint32_t>(1U, width - 1U));
      const auto r = static_cast<std::uint8_t>(20U + h);
      const auto g = static_cast<std::uint8_t>(28U + v);
      const auto b = static_cast<std::uint8_t>(40U + h + v);
      row[x] = (static_cast<std::uint32_t>(r) << 16U) | (static_cast<std::uint32_t>(g) << 8U) | b;
    }
  }
}

// Tile painter: filled colour with a 6-px contrasting border so the
// tile's footprint reads even when the user pushes alpha down toward
// transparency.
void paint_tile(drm::BufferMapping& map, std::uint32_t fill_argb) noexcept {
  const auto width = map.width();
  const auto height = map.height();
  const auto stride_bytes = map.stride();
  const auto pixels = map.pixels();
  fill_argb_rect(pixels, stride_bytes, width, height, fill_argb);
  constexpr std::uint32_t k_border_px = 6U;
  if (width <= k_border_px * 2U || height <= k_border_px * 2U) {
    return;
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    constexpr std::uint32_t border_argb = 0xFF202020U;
    auto* row = reinterpret_cast<std::uint32_t*>(pixels.data() +
                                                 (static_cast<std::size_t>(y) * stride_bytes));
    if (y < k_border_px || y >= height - k_border_px) {
      for (std::uint32_t x = 0; x < width; ++x) {
        row[x] = border_argb;
      }
    } else {
      for (std::uint32_t x = 0; x < k_border_px; ++x) {
        row[x] = border_argb;
      }
      for (std::uint32_t x = width - k_border_px; x < width; ++x) {
        row[x] = border_argb;
      }
    }
  }
}

drm::expected<void, std::error_code> paint_dumb_source(drm::scene::DumbBufferSource& src,
                                                       std::uint32_t fill_argb) {
  auto m = src.map(drm::MapAccess::Write);
  if (!m) {
    return drm::unexpected<std::error_code>(m.error());
  }
  paint_tile(*m, fill_argb);
  return {};
}

void reset_tile_state(Tile& t, std::uint32_t fb_w, std::uint32_t fb_h, std::uint32_t index) {
  // Stagger the eight tiles diagonally across the screen so a fresh
  // "all on" state spreads them out instead of stacking them.
  const std::uint32_t cols = 4U;
  const std::uint32_t rows = 2U;
  const std::uint32_t col = index % cols;
  const std::uint32_t row = index / cols;
  const std::uint32_t cell_w = fb_w / cols;
  const std::uint32_t cell_h = fb_h / rows;
  const std::int32_t pad_x =
      (static_cast<std::int32_t>(cell_w) - static_cast<std::int32_t>(k_tile_w)) / 2;
  const std::int32_t pad_y =
      (static_cast<std::int32_t>(cell_h) - static_cast<std::int32_t>(k_tile_h)) / 2;
  t.x = static_cast<std::int32_t>(col * cell_w) + std::max<std::int32_t>(0, pad_x);
  t.y = static_cast<std::int32_t>(row * cell_h) + std::max<std::int32_t>(0, pad_y);
  t.zpos = k_zpos_min + static_cast<std::int32_t>(index);
  t.alpha = 0xFFFFU;
  t.color = k_tile_palette.at(index);
}

drm::expected<void, std::error_code> activate(Tile& t, drm::Device& dev,
                                              drm::scene::LayerScene& scene) {
  auto src = drm::scene::DumbBufferSource::create(dev, k_tile_w, k_tile_h, DRM_FORMAT_ARGB8888);
  if (!src) {
    return drm::unexpected<std::error_code>(src.error());
  }
  if (auto r = paint_dumb_source(**src, t.color); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }

  drm::scene::LayerDesc desc;
  desc.source = std::move(*src);
  desc.display.src_rect = drm::scene::Rect{0, 0, k_tile_w, k_tile_h};
  desc.display.dst_rect = drm::scene::Rect{t.x, t.y, k_tile_w, k_tile_h};
  desc.display.zpos = t.zpos;
  desc.display.alpha = t.alpha;
  desc.content_type = drm::planes::ContentType::UI;
  auto h = scene.add_layer(std::move(desc));
  if (!h) {
    return drm::unexpected<std::error_code>(h.error());
  }
  t.handle = *h;
  t.active = true;
  return {};
}

void deactivate(Tile& t, drm::scene::LayerScene& scene) noexcept {
  if (!t.active) {
    return;
  }
  scene.remove_layer(t.handle);
  t.handle = {};
  t.active = false;
}

}  // namespace

int main(int argc, char** argv) {
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

  drm::examples::warn_compat(drm::examples::probe_output(dev, crtc_id),
                             {.wants_alpha_overlays = true, .wants_explicit_zpos = true});

  auto bg_src = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src) {
    drm::println(stderr, "DumbBufferSource::create (bg): {}", bg_src.error().message());
    return EXIT_FAILURE;
  }
  auto* bg = bg_src->get();
  if (auto m = bg->map(drm::MapAccess::Write); m) {
    paint_bg_gradient(*m);
  } else {
    drm::println(stderr, "bg map: {}", m.error().message());
    return EXIT_FAILURE;
  }

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = crtc_id;
  cfg.connector_id = connector_id;
  cfg.mode = mode;
  auto scene_res = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_res) {
    drm::println(stderr, "LayerScene::create: {}", scene_res.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_res);

  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_src);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.content_type = drm::planes::ContentType::Generic;
  if (auto h = scene->add_layer(std::move(bg_desc)); !h) {
    drm::println(stderr, "add bg layer: {}", h.error().message());
    return EXIT_FAILURE;
  }

  std::array<Tile, k_tile_count> tiles{};
  for (std::uint32_t i = 0; i < k_tile_count; ++i) {
    reset_tile_state(tiles.at(i), fb_w, fb_h, i);
  }
  std::size_t selection = 0;
  drm::scene::CommitReport last_report{};

  bool session_paused = false;
  bool flip_pending = false;
  int pending_resume_fd = -1;
  bool dirty = false;

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

  auto cycle_selection = [&]() {
    for (std::size_t step = 1; step <= k_tile_count; ++step) {
      const std::size_t idx = (selection + step) % k_tile_count;
      if (tiles.at(idx).active) {
        selection = idx;
        return;
      }
    }
  };

  auto move_selected = [&](std::int32_t dx, std::int32_t dy) {
    Tile& t = tiles.at(selection);
    if (!t.active) {
      return;
    }
    t.x = std::clamp<std::int32_t>(t.x + dx, 0, static_cast<std::int32_t>(fb_w - k_tile_w));
    t.y = std::clamp<std::int32_t>(t.y + dy, 0, static_cast<std::int32_t>(fb_h - k_tile_h));
    if (auto* live = scene->get_layer(t.handle)) {
      live->set_dst_rect(drm::scene::Rect{t.x, t.y, k_tile_w, k_tile_h});
      dirty = true;
    }
  };

  auto adjust_zpos = [&](std::int32_t delta) {
    Tile& t = tiles.at(selection);
    if (!t.active) {
      return;
    }
    t.zpos = std::clamp(t.zpos + delta, k_zpos_min, k_zpos_max);
    if (auto* live = scene->get_layer(t.handle)) {
      live->set_zpos(t.zpos);
      dirty = true;
    }
  };

  auto adjust_alpha = [&](int delta) {
    Tile& t = tiles.at(selection);
    if (!t.active) {
      return;
    }
    const auto cur = static_cast<int>(t.alpha);
    constexpr auto step = static_cast<int>(k_alpha_step);
    const int next = std::clamp(cur + (delta * step), 0, static_cast<int>(0xFFFFU));
    t.alpha = static_cast<std::uint16_t>(next);
    if (auto* live = scene->get_layer(t.handle)) {
      live->set_alpha(t.alpha);
      dirty = true;
    }
  };

  auto reset_all = [&]() {
    for (std::uint32_t i = 0; i < k_tile_count; ++i) {
      Tile& t = tiles.at(i);
      const bool was_active = t.active;
      if (was_active) {
        deactivate(t, *scene);
      }
      reset_tile_state(t, fb_w, fb_h, i);
      if (was_active) {
        if (auto r = activate(t, dev, *scene); !r) {
          drm::println(stderr, "reset activate({}): {}", i, r.error().message());
        }
      }
    }
    selection = 0;
    dirty = true;
  };

  auto toggle_tile = [&](std::size_t idx) {
    Tile& t = tiles.at(idx);
    if (t.active) {
      deactivate(t, *scene);
    } else {
      if (auto r = activate(t, dev, *scene); !r) {
        drm::println(stderr, "activate tile {}: {}", idx, r.error().message());
        return;
      }
    }
    selection = idx;
    dirty = true;
  };

  auto print_state = [&]() {
    std::size_t active = 0;
    for (const auto& t : tiles) {
      if (t.active) {
        ++active;
      }
    }
    drm::println(
        "[state] active={}/{}, selection={}, last_commit: total={} assigned={} composited={} "
        "unassigned={} buckets={} props={} fbs={} test_commits={}",
        active, k_tile_count, selection, last_report.layers_total, last_report.layers_assigned,
        last_report.layers_composited, last_report.layers_unassigned,
        last_report.composition_buckets, last_report.properties_written, last_report.fbs_attached,
        last_report.test_commits_issued);
  };

  bool quit = false;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event);
    if (ke == nullptr || !ke->pressed) {
      return;
    }
    switch (ke->key) {
      case KEY_ESC:
      case KEY_Q:
        quit = true;
        return;
      case KEY_1:
      case KEY_2:
      case KEY_3:
      case KEY_4:
      case KEY_5:
      case KEY_6:
      case KEY_7:
      case KEY_8: {
        const auto idx = static_cast<std::size_t>(ke->key - KEY_1);
        if (idx < k_tile_count) {
          toggle_tile(idx);
        }
        return;
      }
      case KEY_TAB:
        cycle_selection();
        return;
      case KEY_LEFT:
        move_selected(-k_move_step_px, 0);
        return;
      case KEY_RIGHT:
        move_selected(+k_move_step_px, 0);
        return;
      case KEY_UP:
        move_selected(0, -k_move_step_px);
        return;
      case KEY_DOWN:
        move_selected(0, +k_move_step_px);
        return;
      case KEY_Z:
        adjust_zpos(-1);
        return;
      case KEY_X:
        adjust_zpos(+1);
        return;
      case KEY_LEFTBRACE:
        adjust_alpha(-1);
        return;
      case KEY_RIGHTBRACE:
        adjust_alpha(+1);
        return;
      case KEY_R:
        reset_all();
        return;
      case KEY_F1:
        print_state();
        return;
      default:
        return;
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

  auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip);
  if (!r) {
    drm::println(stderr, "first commit failed: {}", r.error().message());
    return EXIT_FAILURE;
  }
  last_report = *r;
  flip_pending = true;
  drm::println(
      "Running — 1..8 toggle, Tab cycle, arrows move, z/x zpos, [/] alpha, r reset, "
      "F1 stats, Esc/q quit.");

  pollfd pfds[3]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;

  while (!quit) {
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
      // Buffer mappings were torn down on pause; repaint the bg and
      // every active tile against the fresh mappings before committing.
      if (auto m = bg->map(drm::MapAccess::Write); m) {
        paint_bg_gradient(*m);
      }
      for (auto& t : tiles) {
        if (!t.active) {
          continue;
        }
        if (auto* layer = scene->get_layer(t.handle)) {
          if (auto* dbs = dynamic_cast<drm::scene::DumbBufferSource*>(&layer->source())) {
            (void)paint_dumb_source(*dbs, t.color);
          }
        }
      }
      dirty = true;
    }

    if (flip_pending || session_paused) {
      continue;
    }

    if (!dirty) {
      continue;
    }
    dirty = false;

    auto report = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
    if (!report) {
      if (report.error() == std::errc::permission_denied) {
        session_paused = true;
        flip_pending = false;
        continue;
      }
      drm::println(stderr, "commit failed: {}", report.error().message());
      break;
    }
    last_report = *report;
    flip_pending = true;
  }

  return EXIT_SUCCESS;
}