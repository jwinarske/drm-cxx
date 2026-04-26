// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// video_grid — N×N grid of synthesized "video" cells laid out across a
// single output. Each cell owns its own DumbBufferSource and is
// repainted every frame with a diagonal sweeping bar over a flat base
// colour, simulating per-cell motion. The grid stresses the allocator
// (16 layers when 4×4) on hardware whose plane budget is much smaller
// than the cell count, forcing the composition fallback (Phase 2.3) to
// rescue the cells the allocator can't place natively. Cells alternate
// between XRGB8888 and ARGB8888 so the format-cascade path also gets
// exercised.
//
// The runtime layout switch (1/2/3) is the bigger payoff: every switch
// removes every existing layer and adds a fresh set at the new cell
// dimensions, demonstrating that LayerScene's add/remove path handles
// scene churn cleanly across the allocator + composition fallback.
//
// Key bindings:
//   1 — switch to 2×2 grid (4 cells)
//   2 — switch to 3×3 grid (9 cells)
//   3 — switch to 4×4 grid (16 cells)
//   n / Space — next layout (wraps)
//   p — previous layout (wraps)
//   Esc / q — quit

#include "common/open_output.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

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
#include <vector>

namespace {

constexpr std::array<std::uint32_t, 3> k_layouts{2U, 3U, 4U};
constexpr std::uint32_t k_bar_width_px = 12U;

constexpr std::array<std::uint32_t, 16> k_palette{
    0xFFE57373U, 0xFF81C784U, 0xFF64B5F6U, 0xFFFFD54FU, 0xFFBA68C8U, 0xFF4DB6ACU,
    0xFFFF8A65U, 0xFF7986CBU, 0xFFA1887FU, 0xFF90A4AEU, 0xFFAED581U, 0xFF4FC3F7U,
    0xFFFFB74DU, 0xFFF06292U, 0xFF9575CDU, 0xFFDCE775U,
};

struct Cell {
  drm::scene::DumbBufferSource* src{nullptr};
  drm::scene::LayerHandle handle{};
  std::uint32_t base_argb{0};
};

// Rewrites every pixel of the cell. The diagonal bar sweeps to the
// right by `phase` and skews one pixel per row; the result reads as a
// soft motion field at any frame rate. ARGB cells pick up alpha=FF in
// the high byte; XRGB cells get the same byte pattern (hardware ignores
// X bits) so the painter doesn't need to branch on the cell format.
void paint_cell(drm::span<std::uint8_t> pixels, std::uint32_t stride_bytes, std::uint32_t width,
                std::uint32_t height, std::uint32_t base_argb, std::uint32_t bar_argb,
                std::uint32_t phase) noexcept {
  if (width == 0U || height == 0U || stride_bytes < width * 4U) {
    return;
  }
  if (pixels.size() < static_cast<std::size_t>(height) * stride_bytes) {
    return;
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    auto* row = reinterpret_cast<std::uint32_t*>(pixels.data() +
                                                 (static_cast<std::size_t>(y) * stride_bytes));
    const std::uint32_t bar_start = (y + phase) % width;
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint32_t rel = (x + width - bar_start) % width;
      row[x] = (rel < k_bar_width_px) ? bar_argb : base_argb;
    }
  }
}

drm::expected<std::vector<Cell>, std::error_code> build_cells(drm::Device& dev,
                                                              drm::scene::LayerScene& scene,
                                                              std::uint32_t fb_w,
                                                              std::uint32_t fb_h, std::uint32_t n) {
  std::vector<Cell> cells;
  const std::uint32_t cell_w = fb_w / n;
  const std::uint32_t cell_h = fb_h / n;
  if (cell_w == 0U || cell_h == 0U) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  cells.reserve(static_cast<std::size_t>(n) * n);
  for (std::uint32_t row = 0; row < n; ++row) {
    for (std::uint32_t col = 0; col < n; ++col) {
      const std::uint32_t idx = (row * n) + col;
      const bool argb = (idx & 1U) != 0U;
      const std::uint32_t fmt = argb ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
      auto src = drm::scene::DumbBufferSource::create(dev, cell_w, cell_h, fmt);
      if (!src) {
        return drm::unexpected<std::error_code>(src.error());
      }
      auto* src_ptr = src->get();
      drm::scene::LayerDesc desc;
      desc.source = std::move(*src);
      desc.display.src_rect = drm::scene::Rect{0, 0, cell_w, cell_h};
      desc.display.dst_rect =
          drm::scene::Rect{static_cast<std::int32_t>(col * cell_w),
                           static_cast<std::int32_t>(row * cell_h), cell_w, cell_h};
      // Cells don't overlap, so leave zpos unset and let the allocator
      // sort plane assignment. Forcing zpos here would interact badly
      // with amdgpu's PRIMARY-pinned-at-2 quirk on bigger grids.
      desc.content_type = drm::planes::ContentType::Video;
      auto h = scene.add_layer(std::move(desc));
      if (!h) {
        return drm::unexpected<std::error_code>(h.error());
      }
      cells.push_back(Cell{src_ptr, *h, k_palette.at(idx % k_palette.size())});
    }
  }
  return cells;
}

void paint_all(std::vector<Cell>& cells, std::uint32_t cell_w, std::uint32_t cell_h,
               std::uint32_t phase) noexcept {
  for (auto& c : cells) {
    paint_cell(c.src->pixels(), c.src->stride(), cell_w, cell_h, c.base_argb, 0xFFFFFFFFU, phase);
  }
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

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = crtc_id;
  cfg.connector_id = connector_id;
  cfg.mode = mode;
  auto scene_res = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_res) {
    drm::println(stderr, "LayerScene::create failed: {}", scene_res.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_res);

  std::size_t layout_idx = 0;
  std::uint32_t grid_n = k_layouts.at(layout_idx);
  std::uint32_t cell_w = fb_w / grid_n;
  std::uint32_t cell_h = fb_h / grid_n;
  std::uint32_t phase = 0U;

  auto cells_res = build_cells(dev, *scene, fb_w, fb_h, grid_n);
  if (!cells_res) {
    drm::println(stderr, "build_cells failed: {}", cells_res.error().message());
    return EXIT_FAILURE;
  }
  auto cells = std::move(*cells_res);
  paint_all(cells, cell_w, cell_h, phase);

  bool session_paused = false;
  bool flip_pending = false;
  int pending_resume_fd = -1;

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
  std::optional<std::size_t> pending_layout;
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
        pending_layout = 0;
        return;
      case KEY_2:
        pending_layout = 1;
        return;
      case KEY_3:
        pending_layout = 2;
        return;
      case KEY_N:
      case KEY_SPACE:
        pending_layout = (layout_idx + 1U) % k_layouts.size();
        return;
      case KEY_P:
        pending_layout = (layout_idx + k_layouts.size() - 1U) % k_layouts.size();
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

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit failed: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flip_pending = true;
  drm::println("Running {}×{} grid ({} cells) — 1/2/3 select layout, n/p step, Esc/q quit.", grid_n,
               grid_n, cells.size());

  pollfd pfds[3]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;

  while (!quit) {
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
      // Buffer mappings were torn down on pause; repaint every cell
      // before the next commit so the resumed scanout has fresh pixels.
      paint_all(cells, cell_w, cell_h, phase);
    }

    if (flip_pending || session_paused) {
      continue;
    }

    if (pending_layout.has_value() && *pending_layout != layout_idx) {
      layout_idx = *pending_layout;
      pending_layout.reset();
      for (auto& c : cells) {
        scene->remove_layer(c.handle);
      }
      cells.clear();
      grid_n = k_layouts.at(layout_idx);
      cell_w = fb_w / grid_n;
      cell_h = fb_h / grid_n;
      auto fresh = build_cells(dev, *scene, fb_w, fb_h, grid_n);
      if (!fresh) {
        drm::println(stderr, "rebuild ({}×{}): {}", grid_n, grid_n, fresh.error().message());
        break;
      }
      cells = std::move(*fresh);
      drm::println("Layout: {}×{} ({} cells)", grid_n, grid_n, cells.size());
    } else {
      pending_layout.reset();
    }

    ++phase;
    if (phase >= cell_w) {
      phase = 0U;
    }
    paint_all(cells, cell_w, cell_h, phase);

    if (auto report =
            scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
        !report) {
      if (report.error() == std::errc::permission_denied) {
        session_paused = true;
        flip_pending = false;
        continue;
      }
      drm::println(stderr, "commit failed: {}", report.error().message());
      break;
    }
    flip_pending = true;
  }

  return EXIT_SUCCESS;
}