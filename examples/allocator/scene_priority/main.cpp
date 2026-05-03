// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// scene_priority — pedagogical demonstrator for the allocator's
// content-type / refresh-rate-driven priority eviction.
//
// What you should see when this runs:
//
//   Layer  0 priority=100 (Video, 60Hz)        → wants hardware
//   Layer  1 priority=100 (Video, 60Hz)        → wants hardware
//   Layer  2 priority= 80 (UI,    60Hz)        → wants hardware
//   Layer  3 priority= 80 (UI,    60Hz)        → wants hardware
//   Layer  4 priority= 50 (UI,    30Hz)        → may evict to canvas
//   Layer  5 priority= 50 (UI,    30Hz)        → may evict to canvas
//   Layer  6 priority= 10 (Generic, 0Hz)       → likely composited
//   Layer  7 priority= 10 (Generic, 0Hz)       → likely composited
//
//   frame   1 assigned=4 composited=4 unassigned=0  ← cold start
//   frame   2 assigned=4 composited=4 unassigned=0
//   ...
//
// The story: when more layers exist than hardware planes can host
// (8 layers on 3–6 typical KMS planes), the allocator must choose
// which to place directly and which to evict. The eviction order
// is priority-driven: drm::planes::Allocator::layer_priority()
// returns 100 for ContentType::Video, 80 for layers with an
// update_hint_hz > 30, 50 for layers with any update_hint_hz, and
// 10 for everything else. Higher-priority layers keep their hardware
// plane; lower-priority layers spill to the composition canvas (or,
// when even the canvas plane can't be reserved, get dropped for the
// frame and surface in CommitReport::layers_unassigned).
//
// The example ships eight layers laid out in a 4x2 grid with
// distinct solid-color fills, each tagged with a priority intent
// in the comments below. On a typical amdgpu desktop the scene
// has access to PRIMARY plus three or four OVERLAY slots minus
// one reserved for the canvas — i.e. two to four hardware slots
// for layers, the rest spilling to the composition canvas. With
// eight layers, the higher-priority Video / UI-60Hz tiles should
// keep their hardware planes and the lower-priority UI-30Hz /
// Generic tiles should land on the canvas. Driver-specific plane
// budgets shift the exact split — the printed report makes the
// actual numbers visible.
//
// Pure consumer of the existing scene API. Static solid-fill
// buffers, painted once before the loop. Buffer dirtying would
// muddle the priority signal we're trying to expose; layer
// content is invariant for the run.
//
// Usage: scene_priority [/dev/dri/cardN]

#include "../../common/format_probe.hpp"
#include "../../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace {

void fill_solid(drm::BufferMapping& map, std::uint8_t b, std::uint8_t g, std::uint8_t r,
                std::uint8_t a) {
  const auto pixels = map.pixels();
  const auto stride = map.stride();
  for (std::uint32_t y = 0; y < map.height(); ++y) {
    auto* row = pixels.data() + (static_cast<std::size_t>(y) * stride);
    for (std::uint32_t x = 0; x < map.width(); ++x) {
      row[(x * 4U) + 0U] = b;
      row[(x * 4U) + 1U] = g;
      row[(x * 4U) + 2U] = r;
      row[(x * 4U) + 3U] = a;
    }
  }
}

// One row in the layer table below — describes a tile in the 4x2 grid.
struct TileSpec {
  drm::planes::ContentType content;
  std::uint32_t update_hz;  // refresh-rate hint that drives priority
  std::uint8_t b, g, r;     // fill color (BGRX byte order)
  const char* description;  // for the priority listing at startup
};

// Computed priority — mirrors drm::planes::Allocator::layer_priority's
// internal table so the example can print "what the allocator will see"
// without taking a dependency on a private library function.
constexpr int priority_for(const TileSpec& t) {
  if (t.content == drm::planes::ContentType::Video) {
    return 100;
  }
  if (t.update_hz > 30U) {
    return 80;
  }
  if (t.update_hz > 0U) {
    return 50;
  }
  return 10;
}

}  // namespace

int main(int argc, char* argv[]) {
  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const drmModeModeInfo mode = output->mode;
  const std::uint32_t fb_w = mode.hdisplay;
  const std::uint32_t fb_h = mode.vdisplay;
  drm::println("Modeset: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h, mode.vrefresh,
               output->connector_id, output->crtc_id);

  drm::examples::warn_compat(
      drm::examples::probe_output(dev, output->crtc_id),
      {.wants_alpha_overlays = true, .wants_explicit_zpos = true, .wants_overlay_count = 3U});

  // ── The eight layers ──────────────────────────────────────────────
  // Two of each priority class so eviction has unambiguous tiebreakers.
  // BGR colors picked to be visually distinct on a typical desktop.
  constexpr std::array<TileSpec, 8> tiles = {{
      {drm::planes::ContentType::Video, 60U, 0x00, 0x00, 0xFF, "Video, 60Hz"},
      {drm::planes::ContentType::Video, 60U, 0x00, 0x80, 0xFF, "Video, 60Hz"},
      {drm::planes::ContentType::UI, 60U, 0x00, 0xFF, 0xFF, "UI,    60Hz"},
      {drm::planes::ContentType::UI, 60U, 0x00, 0xFF, 0x80, "UI,    60Hz"},
      {drm::planes::ContentType::UI, 30U, 0x80, 0xFF, 0x00, "UI,    30Hz"},
      {drm::planes::ContentType::UI, 30U, 0xFF, 0xFF, 0x00, "UI,    30Hz"},
      {drm::planes::ContentType::Generic, 0U, 0xFF, 0x80, 0x00, "Generic, 0Hz"},
      {drm::planes::ContentType::Generic, 0U, 0xFF, 0x00, 0x80, "Generic, 0Hz"},
  }};

  drm::println("Layer priority intent:");
  for (std::size_t i = 0; i < tiles.size(); ++i) {
    const auto& t = tiles.at(i);
    drm::println("  Layer {} priority={:>3} ({})", i, priority_for(t), t.description);
  }

  // ── Scene + 4x2 grid of tiles ─────────────────────────────────────
  const drm::scene::LayerScene::Config cfg{output->crtc_id, output->connector_id, mode};
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  // Tiles laid out non-overlapping in a 4x2 grid, half-screen-wide
  // each, occupying a horizontal strip 60% of screen height. zpos
  // sits at 3..10 to clear amdgpu's PRIMARY=2 pin (memory:
  // reference_amdgpu_primary_zpos_pin); spacing them on distinct zpos
  // values gives the allocator deterministic stacking when it does
  // need to fall back to compositing.
  const std::uint32_t tile_w = fb_w / 4U;
  const std::uint32_t tile_h = (fb_h * 6U) / 20U;  // 0.3 of screen height
  const auto y0 = static_cast<std::int32_t>(fb_h / 5U);

  for (std::size_t i = 0; i < tiles.size(); ++i) {
    const auto& t = tiles.at(i);
    auto src = drm::scene::DumbBufferSource::create(dev, tile_w, tile_h, DRM_FORMAT_ARGB8888);
    if (!src) {
      drm::println(stderr, "DumbBufferSource(layer {}): {}", i, src.error().message());
      return EXIT_FAILURE;
    }
    {
      auto m = src->get()->map(drm::MapAccess::Write);
      if (!m) {
        drm::println(stderr, "map(layer {}): {}", i, m.error().message());
        return EXIT_FAILURE;
      }
      fill_solid(*m, t.b, t.g, t.r, 0xC0);  // 75% alpha
    }

    const auto col = static_cast<std::int32_t>(i % 4U);
    const auto row = static_cast<std::int32_t>(i / 4U);
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = {0, 0, tile_w, tile_h};
    desc.display.dst_rect = {col * static_cast<std::int32_t>(tile_w),
                             y0 + (row * static_cast<std::int32_t>(tile_h)), tile_w, tile_h};
    desc.display.zpos = 3 + static_cast<int>(i);
    desc.content_type = t.content;
    desc.update_hint_hz = t.update_hz;
    if (auto r = scene->add_layer(std::move(desc)); !r) {
      drm::println(stderr, "add_layer({}): {}", i, r.error().message());
      return EXIT_FAILURE;
    }
  }

  // ── Page-flip plumbing + steady-state loop ────────────────────────
  drm::PageFlip page_flip(dev);
  bool flip_pending = false;
  page_flip.set_handler([&](std::uint32_t /*crtc*/, std::uint64_t /*seq*/,
                            std::uint64_t /*ts_ns*/) noexcept { flip_pending = false; });

  // 30 frames is enough to see the steady-state placement; the priority
  // signal is constant across the run because content_type and
  // update_hint_hz are immutable post-create. We're not exercising
  // priority *change* here — that's a future scene_priority_change
  // example if it proves useful.
  constexpr int k_frames = 30;
  for (int frame = 1; frame <= k_frames; ++frame) {
    while (flip_pending) {
      if (auto r = page_flip.dispatch(-1); !r) {
        drm::println(stderr, "page_flip dispatch: {}", r.error().message());
        return EXIT_FAILURE;
      }
    }
    auto report = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip);
    if (!report) {
      drm::println(stderr, "commit (frame {}): {}", frame, report.error().message());
      return EXIT_FAILURE;
    }
    flip_pending = true;

    drm::println("frame {:>3} assigned={} composited={} unassigned={} (test_commits={})", frame,
                 report->layers_assigned, report->layers_composited, report->layers_unassigned,
                 report->test_commits_issued);

    // Per-layer placement table — printed once after the first
    // successful commit when the allocator's choices have settled.
    // The aggregate counters above tell you the shape of the result;
    // this table tells you which specific layer landed on which
    // specific plane, and whether it got there directly or via the
    // canvas.
    if (frame == 1) {
      drm::println("Per-layer placement (commit {}):", frame);
      for (std::size_t i = 0; i < report->placements.size() && i < tiles.size(); ++i) {
        const auto& p = report->placements.at(i);
        const char* kind = "dropped";
        if (p.placement == drm::scene::LayerPlacement::AssignedToPlane) {
          kind = "assigned";
        } else if (p.placement == drm::scene::LayerPlacement::Composited) {
          kind = "composited";
        }
        if (p.placement == drm::scene::LayerPlacement::Unassigned) {
          drm::println("  Layer {} ({}) → {}", i, tiles.at(i).description, kind);
        } else {
          drm::println("  Layer {} ({}) → plane {} ({})", i, tiles.at(i).description, p.plane_id,
                       kind);
        }
      }
    }
  }

  while (flip_pending) {
    if (auto r = page_flip.dispatch(-1); !r) {
      drm::println(stderr, "page_flip dispatch (drain): {}", r.error().message());
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
