// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// scene_warm_start — pedagogical demonstrator for the allocator's
// warm-start optimization.
//
// What you should see when this runs:
//
//   frame  1 test_commits= 1 assigned=3 ... props= 32 fbs=3   ← cold
//   frame  2 test_commits= 1 assigned=3 ... props=  3 fbs=3   ← warm
//   frame  3 test_commits= 1 assigned=3 ... props=  3 fbs=3   ← steady
//   ...
//   frame 30 test_commits= 1 assigned=3 ... props=  4 fbs=3
//   ↑ HUD translated 16px — dirty layer, warm TEST still validates
//   frame 31 test_commits= 1 assigned=3 ... props=  3 fbs=3   ← steady
//   ...
//
// The visible cold-vs-warm signal here is `props`, not `test_commits`:
//
//   - Frame 1 writes the full plane state for every assigned plane —
//     CRTC_ID, FB_ID, src/dst rects, zpos, alpha, plus the mode blob —
//     so `props` is large (32 for this 3-layer scene on amdgpu).
//   - Frame 2+ only writes the FB_ID delta for double-buffering, so
//     `props` collapses to 3 (one FB_ID per layer).
//   - Move frames (30, 50) write one extra dst_rect prop on the dirtied
//     HUD layer, so `props` ticks up to 4.
//
// `test_commits` stays at 1 throughout — including frame 1 — because
// the allocator's bipartite preseed solves this 3-layer scene on the
// first probe and the full_search ladder never has to fall back to
// greedy or backtracking. A more adversarial scene (tight format /
// scaling / zpos constraints) would defeat the preseed and you'd see
// `test_commits` spike on frame 1 as the ladder steps through each
// rung, each rung issuing one DRM_MODE_ATOMIC_TEST_ONLY probe.
//
// Steady-state `test_commits=1` is the warm-start invariant: the
// allocator caches the previous frame's plane→layer assignment and
// re-validates it with a single TEST per commit. When a layer is
// dirtied, the warm path still runs — for changes the kernel accepts
// on the same plane→layer mapping (pure translation, alpha tweaks)
// the TEST passes and we never re-enter full_search. A change the
// cached assignment can't satisfy — formats, sizes that exceed the
// plane's scaling, zpos values outside the plane's range — fails
// the warm TEST and kicks full_search back in, briefly spiking
// `test_commits`.
//
// The example ships exactly three layers (bg + indicator + HUD) and
// two 16-pixel HUD translations (frames 30, 50) to exercise the
// dirty-but-still-warm path. Buffer contents are static solid fills
// — repainting a buffer would also be a dirty event but would muddle
// the warm-start signal we're trying to expose. Printing is to
// stderr via drm::println.
//
// Usage: scene_warm_start [/dev/dri/cardN]

#include "../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace {

// Solid-color fill in XRGB / ARGB byte order (little-endian on x86:
// memory is B,G,R,X). Caller passes the desired (B,G,R,A) channels;
// for opaque XRGB, A is the X byte (typically 0).
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

  // ── Layer sources ──────────────────────────────────────────────────
  // bg  — full-screen XRGB8888, dark gray.
  // ind — 400×200 ARGB8888 panel near the centre, semi-transparent.
  // hud — 200×60 ARGB8888 badge, the only thing that moves.
  auto bg_src = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src) {
    drm::println(stderr, "DumbBufferSource(bg): {}", bg_src.error().message());
    return EXIT_FAILURE;
  }
  auto ind_src = drm::scene::DumbBufferSource::create(dev, 400, 200, DRM_FORMAT_ARGB8888);
  if (!ind_src) {
    drm::println(stderr, "DumbBufferSource(ind): {}", ind_src.error().message());
    return EXIT_FAILURE;
  }
  auto hud_src = drm::scene::DumbBufferSource::create(dev, 200, 60, DRM_FORMAT_ARGB8888);
  if (!hud_src) {
    drm::println(stderr, "DumbBufferSource(hud): {}", hud_src.error().message());
    return EXIT_FAILURE;
  }

  // Paint each buffer once. Solid fills are deliberate — repainting in
  // the loop would dirty the layer and obscure the warm-start signal.
  {
    auto m = bg_src->get()->map(drm::MapAccess::Write);
    if (!m) {
      drm::println(stderr, "bg map: {}", m.error().message());
      return EXIT_FAILURE;
    }
    fill_solid(*m, 0x20, 0x20, 0x20, 0x00);  // dark gray
  }
  {
    auto m = ind_src->get()->map(drm::MapAccess::Write);
    if (!m) {
      drm::println(stderr, "ind map: {}", m.error().message());
      return EXIT_FAILURE;
    }
    fill_solid(*m, 0x60, 0x80, 0x00, 0x80);  // teal, alpha 0.50
  }
  {
    auto m = hud_src->get()->map(drm::MapAccess::Write);
    if (!m) {
      drm::println(stderr, "hud map: {}", m.error().message());
      return EXIT_FAILURE;
    }
    fill_solid(*m, 0x00, 0x80, 0xFF, 0xC0);  // orange, alpha 0.75
  }

  // ── Scene + layer registration ─────────────────────────────────────
  const drm::scene::LayerScene::Config cfg{output->crtc_id, output->connector_id, mode};
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  // bg fills the screen. No explicit zpos — the allocator places it on
  // PRIMARY and amdgpu's immutable PRIMARY zpos (=2) sits below the
  // overlays we add next.
  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_src);
  bg_desc.display.src_rect = {0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = {0, 0, fb_w, fb_h};
  if (auto r = scene->add_layer(std::move(bg_desc)); !r) {
    drm::println(stderr, "add_layer(bg): {}", r.error().message());
    return EXIT_FAILURE;
  }

  // ind centred. zpos=3 to clear amdgpu's PRIMARY=2 (memory:
  // reference_amdgpu_primary_zpos_pin).
  drm::scene::LayerDesc ind_desc;
  ind_desc.source = std::move(*ind_src);
  const auto ind_x = static_cast<std::int32_t>((fb_w / 2U) - 200U);
  const auto ind_y = static_cast<std::int32_t>((fb_h / 2U) - 100U);
  ind_desc.display.src_rect = {0, 0, 400, 200};
  ind_desc.display.dst_rect = {ind_x, ind_y, 400, 200};
  ind_desc.display.zpos = 3;
  if (auto r = scene->add_layer(std::move(ind_desc)); !r) {
    drm::println(stderr, "add_layer(ind): {}", r.error().message());
    return EXIT_FAILURE;
  }

  // hud top-left, zpos above ind. Capture its handle — the move event
  // mutates this layer's dst_rect.
  drm::scene::LayerDesc hud_desc;
  hud_desc.source = std::move(*hud_src);
  std::int32_t hud_x = 32;
  hud_desc.display.src_rect = {0, 0, 200, 60};
  hud_desc.display.dst_rect = {hud_x, 32, 200, 60};
  hud_desc.display.zpos = 4;
  auto hud_handle_r = scene->add_layer(std::move(hud_desc));
  if (!hud_handle_r) {
    drm::println(stderr, "add_layer(hud): {}", hud_handle_r.error().message());
    return EXIT_FAILURE;
  }
  const drm::scene::LayerHandle hud_handle = *hud_handle_r;

  // ── Page-flip plumbing ─────────────────────────────────────────────
  drm::PageFlip page_flip(dev);
  bool flip_pending = false;
  page_flip.set_handler([&](std::uint32_t /*crtc*/, std::uint64_t /*seq*/,
                            std::uint64_t /*ts_ns*/) noexcept { flip_pending = false; });

  // ── Main loop ──────────────────────────────────────────────────────
  // 60 frames is enough to show: cold start, settle, two move events,
  // and the warm-up after each. Move events at frames 30 and 50 — the
  // gap is intentional so each spike sits in clean context.
  constexpr int k_frames = 60;
  constexpr int k_move_frame_a = 30;
  constexpr int k_move_frame_b = 50;
  for (int frame = 1; frame <= k_frames; ++frame) {
    const bool moved = (frame == k_move_frame_a || frame == k_move_frame_b);
    if (moved) {
      hud_x += 16;
      auto* hud = scene->get_layer(hud_handle);
      if (hud == nullptr) {
        drm::println(stderr, "get_layer(hud) returned null");
        return EXIT_FAILURE;
      }
      hud->set_dst_rect({hud_x, 32, 200, 60});
    }

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

    drm::println(
        "frame {:>3} test_commits={:>2} assigned={} composited={} unassigned={} "
        "props={:>3} fbs={}",
        frame, report->test_commits_issued, report->layers_assigned, report->layers_composited,
        report->layers_unassigned, report->properties_written, report->fbs_attached);
    if (moved) {
      drm::println("  ↑ HUD translated 16px — dirty layer, warm TEST still validates");
    }
  }

  // Drain the trailing flip so we exit cleanly rather than killing a
  // commit the kernel hasn't completed yet.
  while (flip_pending) {
    if (auto r = page_flip.dispatch(-1); !r) {
      drm::println(stderr, "page_flip dispatch (drain): {}", r.error().message());
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
