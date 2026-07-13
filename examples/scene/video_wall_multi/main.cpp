// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// video_wall_multi — N×M grid of synthesized "video" cells laid out
// across every connected output on one card. Logically one wall: the
// total width is the sum of every output's hdisplay; vertical extent
// is the first output's vdisplay. Cells that fit inside one output go
// to that output's LayerScene; cells that straddle a boundary register
// two targets on a single shared LayerBufferSource via SceneSet::add_layer,
// each target carrying a sub-rect of the cell content sized to the
// portion that lands on its scene.
//
// Stresses three things at once on a multi-CRTC setup:
//
//   * Cross-CRTC atomic commits.  Every frame issues one (or two,
//     when NarrowPolicy::AutoOnModeset splits) drmModeAtomicCommit
//     covering N CRTCs. The wall-spanning bar tracks across the
//     output boundary on the same vblank — visual proof.
//   * Per-CRTC plane budget + composition fallback.  N×M cells on
//     each output overflow typical 3–6 plane budgets; the cells the
//     allocator can't place natively land on each scene's own
//     CompositeCanvas.
//   * SceneSet::add_layer with multi-target specs.  Straddling cells
//     exercise the SharedLayerBufferSource forwarder against a real
//     allocator + commit cycle, not just a TEST_ONLY probe.
//
// Hardware requirements: at least two connected outputs on one card.
// On a single-output workstation, provision vkms:
//
//   sudo scripts/vkms_dual.sh up
//   ./video_wall_multi /dev/dri/cardN     # the vkms node
//
// Keys:
//
//   1 / 2 / 3 — switch to 2×2 / 3×3 / 4×4 logical grid
//   n / Space — next layout (wraps)
//   p         — previous layout (wraps)
//   Esc / q   — quit
//   Ctrl+Alt+F<n> — VT switch (libseat-managed)

#include "../../common/event_loop.hpp"
#include "../../common/multi_crtc_probe.hpp"
#include "../../common/open_output.hpp"
#include "../../common/vt_switch.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/display_params.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/scene_set.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// One logical cell tracked by the application. The DumbBufferSource is
// shared via SceneSet::add_layer; the handle drives remove_layer when
// the layout cycler tears the grid down.
struct Cell {
  drm::scene::DumbBufferSource* src{nullptr};
  std::shared_ptr<drm::scene::LayerBufferSource> shared;
  drm::scene::SetLayerHandle handle{};
  std::uint32_t base_argb{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
};

// One connected output's logical X range on the wall, plus a back-pointer
// to its index in the SceneSet so dispatch can address the right scene.
struct OutputStrip {
  std::size_t scene_index{0};
  std::int32_t x_start{0};    // logical wall x where this output begins
  std::uint32_t hdisplay{0};  // output's KMS hdisplay
  std::uint32_t vdisplay{0};  // output's KMS vdisplay
};

// Same painter shape as video_grid: diagonal sweeping bar over a flat
// base color, computed off `phase` so the frame-to-frame delta is just
// the bar position.
void paint_cell(drm::BufferMapping& map, std::uint32_t base_argb, std::uint32_t bar_argb,
                std::uint32_t phase) noexcept {
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
    auto* row = reinterpret_cast<std::uint32_t*>(pixels.data() +
                                                 (static_cast<std::size_t>(y) * stride_bytes));
    const std::uint32_t bar_start = (y + phase) % width;
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint32_t rel = (x + width - bar_start) % width;
      row[x] = (rel < k_bar_width_px) ? bar_argb : base_argb;
    }
  }
}

void paint_all(std::vector<Cell>& cells, std::uint32_t phase) noexcept {
  for (auto& c : cells) {
    auto m = c.src->map(drm::MapAccess::Write);
    if (!m) {
      continue;
    }
    paint_cell(*m, c.base_argb, 0xFFFFFFFFU, phase);
  }
}

// Build the per-output strip table from the enumerated outputs. Strips
// are laid left-to-right in the order outputs were enumerated.
std::vector<OutputStrip> build_strips(
    drm::span<const drm::examples::multi_crtc::ConnectedOutput> outputs) {
  std::vector<OutputStrip> strips;
  strips.reserve(outputs.size());
  std::int32_t cursor = 0;
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    strips.push_back({.scene_index = i,
                      .x_start = cursor,
                      .hdisplay = outputs[i].mode.hdisplay,
                      .vdisplay = outputs[i].mode.vdisplay});
    cursor += static_cast<std::int32_t>(outputs[i].mode.hdisplay);
  }
  return strips;
}

// Scan argv for a bare flag (e.g. `--list-outputs`). True if present.
bool has_flag(int argc, char** argv, const char* flag) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], flag) == 0) {
      return true;
    }
  }
  return false;
}

// Print each connected output's connector name + mode + CRTC, in DRM
// enumeration order — the order `--order` reorders against. Intended
// as a one-shot helper: the user runs the example with
// `--list-outputs`, copies the names into `--order <left>,<right>,...`,
// then runs again for real.
void print_outputs(drm::span<const drm::examples::multi_crtc::ConnectedOutput> outputs) {
  drm::println("Connected outputs (DRM enumeration order):");
  for (const auto& o : outputs) {
    drm::println("  {} @ CRTC {} mode {}x{}@{}Hz", o.connector_name, o.crtc_id, o.mode.hdisplay,
                 o.mode.vdisplay, o.mode.vrefresh);
  }
  if (outputs.size() >= 2) {
    drm::println("Use e.g. --order {},{} to override left-to-right physical order.",
                 outputs[0].connector_name, outputs[1].connector_name);
  }
}

// Parse `--order <name1>,<name2>,...` out of argv, returning the
// comma-separated list of connector names (e.g. {"DP-1", "HDMI-A-1"})
// or nullopt if the flag is absent. Malformed --order (missing value,
// empty list) returns nullopt with a stderr diagnostic so the caller
// can decide whether to abort.
std::optional<std::vector<std::string>> parse_order_flag(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--order") != 0) {
      continue;
    }
    if (i + 1 >= argc) {
      drm::println(stderr, "--order needs a value (e.g. --order DP-1,HDMI-A-1)");
      return std::vector<std::string>{};
    }
    std::vector<std::string> names;
    std::string_view tail(argv[i + 1]);
    while (!tail.empty()) {
      const auto comma = tail.find(',');
      const auto piece = (comma == std::string_view::npos) ? tail : tail.substr(0, comma);
      if (!piece.empty()) {
        names.emplace_back(piece);
      }
      tail = (comma == std::string_view::npos) ? std::string_view{} : tail.substr(comma + 1);
    }
    return names;
  }
  return std::nullopt;
}

// Reorder the enumerated outputs to match the user's left-to-right
// physical layout. Outputs not named in `names` are appended in their
// original enumeration order; names that don't match any output are
// flagged. Returns false on any mismatch so the caller can abort
// rather than silently mislaying the wall.
bool reorder_outputs(std::vector<drm::examples::multi_crtc::ConnectedOutput>& outputs,
                     drm::span<const std::string> names) {
  std::vector<drm::examples::multi_crtc::ConnectedOutput> reordered;
  reordered.reserve(outputs.size());
  std::vector<bool> consumed(outputs.size(), false);
  bool ok = true;
  for (const auto& name : names) {
    bool matched = false;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      if (!consumed[i] && outputs[i].connector_name == name) {
        reordered.push_back(std::move(outputs[i]));
        consumed[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      drm::println(stderr, "--order: connector '{}' not connected (or already named)", name);
      ok = false;
    }
  }
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    if (!consumed[i]) {
      reordered.push_back(std::move(outputs[i]));
    }
  }
  outputs = std::move(reordered);
  return ok;
}

// Build N×N cells across the logical wall. Each cell's logical
// rectangle is intersected with every strip; intersections produce one
// SceneSetLayerSpec target per strip the cell touches. The shared
// DumbBufferSource keeps painting trivial (one map per cell, regardless
// of how many outputs render it).
drm::expected<std::vector<Cell>, std::error_code> build_cells(
    drm::Device& dev, drm::scene::SceneSet& scene_set, const std::vector<OutputStrip>& strips,
    std::uint32_t grid_n) {
  std::vector<Cell> cells;
  if (strips.empty() || grid_n == 0U) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  const std::uint32_t wall_w =
      static_cast<std::uint32_t>(strips.back().x_start) + strips.back().hdisplay;
  const std::uint32_t wall_h = strips.front().vdisplay;
  const std::uint32_t cell_w = wall_w / grid_n;
  const std::uint32_t cell_h = wall_h / grid_n;
  if (cell_w == 0U || cell_h == 0U) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  cells.reserve(static_cast<std::size_t>(grid_n) * grid_n);
  for (std::uint32_t row = 0; row < grid_n; ++row) {
    for (std::uint32_t col = 0; col < grid_n; ++col) {
      const std::uint32_t idx = (row * grid_n) + col;
      const bool argb = (idx & 1U) != 0U;
      const std::uint32_t fmt = argb ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
      auto src_r = drm::scene::DumbBufferSource::create(dev, cell_w, cell_h, fmt);
      if (!src_r) {
        return drm::unexpected<std::error_code>(src_r.error());
      }
      auto* src_ptr = src_r->get();
      const std::shared_ptr<drm::scene::LayerBufferSource> shared(std::move(*src_r));

      const auto cell_x0 = static_cast<std::int32_t>(col * cell_w);
      const auto cell_x1 = cell_x0 + static_cast<std::int32_t>(cell_w);
      const auto cell_y0 = static_cast<std::int32_t>(row * cell_h);

      drm::scene::SceneSetLayerSpec spec;
      spec.source = shared;

      // Intersect this cell's logical x-range with every strip. Each
      // intersection becomes one target on the strip's scene with the
      // matching src sub-rect and the dst rect translated into the
      // output's local coordinate space.
      for (const auto& strip : strips) {
        const std::int32_t strip_x0 = strip.x_start;
        const auto strip_x1 = strip_x0 + static_cast<std::int32_t>(strip.hdisplay);
        const std::int32_t lo = std::max(cell_x0, strip_x0);
        const std::int32_t hi = std::min(cell_x1, strip_x1);
        if (hi <= lo) {
          continue;
        }
        const auto intersect_w = static_cast<std::uint32_t>(hi - lo);
        const std::int32_t src_x = lo - cell_x0;
        const std::int32_t dst_x = lo - strip_x0;

        drm::scene::DisplayParams disp;
        disp.src_rect = drm::scene::Rect{src_x, 0, intersect_w, cell_h};
        disp.dst_rect = drm::scene::Rect{dst_x, cell_y0, intersect_w, cell_h};
        // amdgpu pins PRIMARY at zpos=2 — keep cells well clear so the
        // composition fallback canvas doesn't collide. idx stagger avoids
        // every-cell-at-the-same-zpos ambiguities the allocator may resolve
        // unstably.
        disp.zpos = 3 + static_cast<int>(idx);
        spec.targets.push_back(
            {.scene_index = strip.scene_index, .display = disp, .force_composited = false});
      }

      if (spec.targets.empty()) {
        // Cell is entirely outside the wall — shouldn't happen with
        // sane strips, but skip rather than fail the whole rebuild.
        continue;
      }

      auto h = scene_set.add_layer(spec);
      if (!h) {
        return drm::unexpected<std::error_code>(h.error());
      }
      cells.push_back(Cell{.src = src_ptr,
                           .shared = shared,
                           .handle = *h,
                           .base_argb = k_palette.at(idx % k_palette.size()),
                           .width = cell_w,
                           .height = cell_h});
    }
  }
  return cells;
}

void clear_cells(drm::scene::SceneSet& scene_set, std::vector<Cell>& cells) {
  for (auto& c : cells) {
    scene_set.remove_layer(c.handle);
  }
  cells.clear();
}

}  // namespace

int main(int argc, char** argv) try {
  auto ctx = drm::examples::open_device(argc, argv);
  if (!ctx) {
    return EXIT_FAILURE;
  }
  auto& dev = ctx->device;
  auto& seat = ctx->seat;

  auto outputs = drm::examples::multi_crtc::enumerate_connected_outputs(dev);

  // `--list-outputs` is a one-shot enumerator — print and exit so the
  // user can copy connector names into a follow-up `--order` invocation
  // without first having to launch the real wall.
  if (has_flag(argc, argv, "--list-outputs")) {
    print_outputs(outputs);
    return EXIT_SUCCESS;
  }

  if (outputs.size() < 2) {
    drm::println(stderr,
                 "video_wall_multi needs >=2 connected outputs (found {}). "
                 "Try `sudo scripts/vkms_dual.sh up` and pass the resulting vkms cardN.",
                 outputs.size());
    return EXIT_FAILURE;
  }

  // Physical layout override: DRM enumeration order doesn't carry
  // physical placement, so users with monitors arranged opposite to
  // the kernel's enumeration order pass `--order <left>,<right>,...`
  // to align the logical wall with the physical wall. Run with
  // `--list-outputs` first to see the connector names you can use.
  if (auto order = parse_order_flag(argc, argv)) {
    if (order->empty()) {
      return EXIT_FAILURE;
    }
    if (!reorder_outputs(outputs, *order)) {
      return EXIT_FAILURE;
    }
  }
  drm::println("Driving {} connected output(s) (left-to-right):", outputs.size());
  for (const auto& o : outputs) {
    drm::println("  {} @ CRTC {} mode {}x{}@{}Hz", o.connector_name, o.crtc_id, o.mode.hdisplay,
                 o.mode.vdisplay, o.mode.vrefresh);
  }

  // Up-front feasibility probe. If a combined commit is rejected by
  // the kernel here, every later commit would fail too — surface that
  // before any allocation.
  const auto probe = drm::examples::multi_crtc::probe_combined_atomic(dev, outputs);
  if (probe.verdict == drm::examples::multi_crtc::CombinedAtomicVerdict::Rejected) {
    drm::println(stderr, "Combined cross-CRTC atomic TEST rejected: {}", probe.error.message());
    return EXIT_FAILURE;
  }

  // Build per-output LayerScenes.
  std::vector<std::unique_ptr<drm::scene::LayerScene>> scenes_owner;
  scenes_owner.reserve(outputs.size());
  for (const auto& o : outputs) {
    drm::scene::LayerScene::Config cfg;
    cfg.crtc_id = o.crtc_id;
    cfg.connector_id = o.connector_id;
    cfg.mode = o.mode;
    auto scene_r = drm::scene::LayerScene::create(dev, cfg);
    if (!scene_r) {
      drm::println(stderr, "LayerScene for {}: {}", o.connector_name, scene_r.error().message());
      return EXIT_FAILURE;
    }
    scenes_owner.push_back(std::move(*scene_r));
  }
  std::vector<drm::scene::LayerScene*> scene_views;
  scene_views.reserve(scenes_owner.size());
  for (auto& s : scenes_owner) {
    scene_views.push_back(s.get());
  }

  const auto strips = build_strips(outputs);

  auto set_r = drm::scene::SceneSet::create(dev, std::move(scenes_owner));
  if (!set_r) {
    drm::println(stderr, "SceneSet::create: {}", set_r.error().message());
    return EXIT_FAILURE;
  }
  auto& scene_set = **set_r;

  std::size_t layout_idx = 0;
  std::uint32_t grid_n = k_layouts.at(layout_idx);
  std::uint32_t phase = 0U;

  auto cells_r = build_cells(dev, scene_set, strips, grid_n);
  if (!cells_r) {
    drm::println(stderr, "build_cells({}x{}): {}", grid_n, grid_n, cells_r.error().message());
    return EXIT_FAILURE;
  }
  auto cells = std::move(*cells_r);
  paint_all(cells, phase);

  // Page-flip coalescing across all CRTCs — same shape as dual_display.
  drm::PageFlip page_flip(dev);
  std::size_t flips_outstanding = 0;
  page_flip.set_handler([&](std::uint32_t /*crtc*/, std::uint64_t /*seq*/, std::uint64_t /*ts*/) {
    if (flips_outstanding > 0) {
      --flips_outstanding;
    }
  });

  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_r = drm::input::Seat::open({}, std::move(libinput_opener));
  if (!input_seat_r) {
    drm::println(stderr, "Failed to open input seat: {}", input_seat_r.error().message());
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_r;
  bool quit = false;
  std::optional<std::size_t> pending_layout;
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
      return;
    }
    if (!ke->pressed) {
      return;
    }
    switch (ke->key) {
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

  // Multi-scene session pump (the common::session_pump.hpp helpers
  // are LayerScene-singular). Filter resume_cb on /dev/dri/ to ignore
  // the input-fd multi-fire path.
  bool session_paused = false;
  int pending_resume_fd = -1;
  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      flips_outstanding = 0;
      for (auto* s : scene_views) {
        s->on_session_paused();
      }
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

  if (auto r = scene_set.commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first SceneSet::commit: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flips_outstanding = scene_set.scene_count();
  drm::println(
      "Running {}×{} grid ({} cells across {} output(s)) — 1/2/3 select, n/p step, Esc/q quit.",
      grid_n, grid_n, cells.size(), strips.size());

  drm::examples::EventLoop loop;
  (void)loop.add_slot(input_seat.fd(), [&] { (void)input_seat.dispatch(); });
  int const drm_slot = loop.add_slot(dev.fd(), [&] { (void)page_flip.dispatch(0); });
  (void)loop.add_slot(seat ? seat->poll_fd() : -1, [&] {
    if (seat) {
      seat->dispatch();
    }
  });

  while (!quit) {
    int timeout = 0;
    if (session_paused) {
      timeout = -1;
    } else if (flips_outstanding > 0) {
      timeout = 16;
    }
    if (!loop.tick(timeout)) {
      break;
    }

    if (pending_resume_fd >= 0) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      dev = drm::Device::from_fd(new_fd);
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes: {}", r.error().message());
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic: {}", r.error().message());
        break;
      }
      bool ok = true;
      for (auto* s : scene_views) {
        if (auto r = s->on_session_resumed(dev); !r) {
          drm::println(stderr, "resume: on_session_resumed: {}", r.error().message());
          ok = false;
          break;
        }
      }
      if (!ok) {
        break;
      }
      paint_all(cells, phase);
      loop.set_fd(drm_slot, dev.fd());
      page_flip = drm::PageFlip(dev);
      page_flip.set_handler([&](std::uint32_t, std::uint64_t, std::uint64_t) {
        if (flips_outstanding > 0) {
          --flips_outstanding;
        }
      });
    }

    if (session_paused || flips_outstanding > 0) {
      continue;
    }

    if (pending_layout.has_value() && *pending_layout != layout_idx) {
      layout_idx = *pending_layout;
      pending_layout.reset();
      clear_cells(scene_set, cells);
      grid_n = k_layouts.at(layout_idx);
      auto fresh = build_cells(dev, scene_set, strips, grid_n);
      if (!fresh) {
        drm::println(stderr, "rebuild ({}×{}): {}", grid_n, grid_n, fresh.error().message());
        break;
      }
      cells = std::move(*fresh);
      drm::println("Layout: {}×{} ({} cells)", grid_n, grid_n, cells.size());
    } else {
      pending_layout.reset();
    }

    // 8 px/frame at 60 Hz — same as video_grid. Cell widths shrink as
    // grid_n grows, but 8 px is still visible at 4×4.
    phase = (phase + 8U) % std::max<std::uint32_t>(1U, cells.empty() ? 1U : cells.front().width);
    paint_all(cells, phase);

    const std::uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
    if (auto r = scene_set.commit(flags, &page_flip); !r) {
      if (r.error() == std::errc::permission_denied) {
        session_paused = true;
        flips_outstanding = 0;
        continue;
      }
      drm::println(stderr, "SceneSet::commit: {}", r.error().message());
      break;
    }
    flips_outstanding = scene_set.scene_count();
  }

  clear_cells(scene_set, cells);
  return EXIT_SUCCESS;
} catch (...) {
  return EXIT_FAILURE;
}
