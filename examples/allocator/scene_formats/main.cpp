// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// scene_formats — pedagogical demonstrator for the allocator's
// bipartite plane-matching across heterogeneous layer requirements.
//
// What you should see when this runs:
//
//   Plane budget for CRTC <id>: <N> usable planes (PRIMARY+OVERLAY,
//                                cursor excluded). Building <K>=min(4,N) layers.
//
//   Layer 0  ARGB8888       1:1  no-scale   → universally supported (baseline)
//   Layer 1  ARGB8888       1:2  scaler     → scaler-capable plane only
//   Layer 2  ABGR8888       1:1  no-scale   → channel-order swap
//   Layer 3  XRGB8888       1:1  no-scale   → universally supported
//
//   frame   1 assigned=N composited=M unassigned=K
//   ...
//
// The story: KMS planes are not interchangeable. Each plane advertises
// a subset of pixel formats (via the IN_FORMATS blob the registry
// parses) and a subset of capabilities (scaling, rotation, modifier
// support). When a scene mixes layer requirements — ARGB here, an
// alternate channel order there, a scaler-required dst_rect on a
// fourth — the allocator has to solve a bipartite assignment: each
// layer must land on a plane that supports its format AND its
// capability needs. A greedy "take the first plane that works for
// this layer" pass fails on contrived but realistic shapes (see the
// plan note on the N+1 problem); drm-cxx's allocator runs an actual
// bipartite solver, so such cases place cleanly when a placement
// exists at all.
//
// The four-layer set this example builds is deliberately mild — all
// four layers are 32bpp (the bound DumbBufferSource imposes), three
// require no scaling, and only one has a non-trivial channel order.
// That keeps the example self-contained and runnable on every driver,
// while still exercising:
//
//   - format diversity (ARGB / XRGB / ABGR — same byte width, distinct
//     channel orderings; not all planes accept all three on every
//     driver, particularly older or constrained-IP embedded SoCs);
//   - scaler matching (the scaler layer's dst_rect is twice the
//     src_rect width, so it can only land on a plane whose
//     IN_FORMATS / capabilities advertise scaling).
//
// Per-driver results vary; the printed CommitReport counts make the
// actual outcome visible.
//
// **Plane-budget awareness.** The full four-layer set fits drivers
// that expose ≥4 PRIMARY+OVERLAY planes per CRTC. On constrained
// pipes (amdgpu DCN typically gives 1 primary + 2 overlays = 3 usable
// planes per CRTC; many embedded SoCs are tighter still), four
// non-overlapping layers can't all land on hardware and the
// allocator falls back to scene-wide composition — `assigned=0
// composited=4` — which masks the demo's pedagogy. The example
// probes the active CRTC's plane budget at startup and trims the
// layer set to `min(4, usable_planes)`. The layer table is ordered
// most-distinctive-first (baseline → scaler → channel-swap →
// redundant-baseline) so each budget keeps the most pedagogically
// interesting subset.
//
// **Out of scope for this example:**
//
//   - YUV (NV12, NV21) layers. These are multi-plane formats requiring
//     a non-32bpp dumb allocation or a GBM-imported buffer. The plan's
//     v2 foreign-buffer-source work (V4L2, accel) is the natural home
//     for a YUV showcase; revisit when GbmBufferSource gains explicit
//     YUV support or a dedicated YUV source type lands.
//
//   - Format modifiers (AFBC, DCC, vendor tilings). LayerDesc::modifier
//     and the allocator's IN_FORMATS-aware eligibility check landed
//     with the format-modifiers PR; demonstrating them needs a buffer
//     producer that can emit tiled output, which DumbBufferSource
//     cannot. A separate scene_modifiers example or a follow-up to
//     this one is the right way to wire that in once a producer is
//     available (GBM with explicit modifier flags is the closest
//     candidate today).
//
// Pure consumer of the existing scene API. Static solid-fill buffers,
// painted once before the loop. Layer content is invariant for the
// run so the placement signal isn't muddled by buffer dirtying.
//
// Usage: scene_formats [/dev/dri/cardN]

#include "../../common/format_probe.hpp"
#include "../../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/plane_registry.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>

namespace {

// Solid-color fill in 32bpp little-endian byte order. The example uses
// three distinct channel orderings; the caller sets channel bytes by
// position rather than name so the same helper paints all three.
void fill_solid(drm::BufferMapping& map, std::uint8_t c0, std::uint8_t c1, std::uint8_t c2,
                std::uint8_t c3) {
  const auto pixels = map.pixels();
  const auto stride = map.stride();
  for (std::uint32_t y = 0; y < map.height(); ++y) {
    auto* row = pixels.data() + (static_cast<std::size_t>(y) * stride);
    for (std::uint32_t x = 0; x < map.width(); ++x) {
      row[(x * 4U) + 0U] = c0;
      row[(x * 4U) + 1U] = c1;
      row[(x * 4U) + 2U] = c2;
      row[(x * 4U) + 3U] = c3;
    }
  }
}

struct LayerSpec {
  std::uint32_t format;
  bool needs_scaling;
  std::uint8_t c0, c1, c2, c3;  // bytes-in-memory order, see fill_solid
  const char* description;
};

const char* format_label(std::uint32_t f) {
  switch (f) {
    case DRM_FORMAT_ARGB8888:
      return "ARGB8888";
    case DRM_FORMAT_XRGB8888:
      return "XRGB8888";
    case DRM_FORMAT_ABGR8888:
      return "ABGR8888";
    case DRM_FORMAT_XBGR8888:
      return "XBGR8888";
    default:
      return "other";
  }
}

// Look up the 0-based CRTC index for `crtc_id` in the device's CRTC
// resource list. PlaneRegistry::for_crtc() and PlaneCapabilities::
// possible_crtcs are both indexed by this position, not the object id.
std::optional<std::uint32_t> crtc_index_of(const drm::Device& dev, std::uint32_t crtc_id) {
  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    return std::nullopt;
  }
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtc_id) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return std::nullopt;
}

// Count PRIMARY+OVERLAY planes reachable from the active CRTC.
// Cursor planes are excluded — they accept only ARGB8888 at fixed
// small sizes and aren't candidates for general layers.
std::size_t count_usable_planes(const drm::Device& dev, std::uint32_t crtc_id) {
  const auto idx = crtc_index_of(dev, crtc_id);
  if (!idx) {
    return 0;
  }
  auto reg = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg) {
    return 0;
  }
  std::size_t n = 0;
  for (const auto* p : reg->for_crtc(*idx)) {
    if (p->type != drm::planes::DRMPlaneType::CURSOR) {
      ++n;
    }
  }
  return n;
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

  drm::examples::warn_compat(drm::examples::probe_output(dev, output->crtc_id),
                             {.wants_alpha_overlays = true, .wants_explicit_zpos = true});

  // Layer table, ordered most-distinctive-first so any plane-budget
  // truncation drops the least pedagogically interesting layers last.
  // Channel-byte values are picked so each layer is visually distinct
  // regardless of channel-order interpretation: ARGB8888 fills
  // (B=0x40, G=0x80, R=0x20, A=0xFF) reads as a green-dominant pixel;
  // ABGR8888 with the same memory layout would read as red-dominant.
  // Driver-side scanout interprets the bytes per format, so a wrong
  // channel order shows up as a tinted strip on the screen and is
  // easy to spot during a TTY run.
  constexpr std::array<LayerSpec, 4> layers = {{
      {DRM_FORMAT_ARGB8888, false, 0x40, 0x80, 0x20, 0xFF, "ARGB8888 1:1 no-scale"},
      {DRM_FORMAT_ARGB8888, true, 0x20, 0x40, 0x80, 0xFF, "ARGB8888 1:2 scaler-required"},
      {DRM_FORMAT_ABGR8888, false, 0x20, 0x80, 0x40, 0xFF, "ABGR8888 1:1 channel-swap"},
      {DRM_FORMAT_XRGB8888, false, 0x80, 0x40, 0x20, 0x00, "XRGB8888 1:1 no-scale"},
  }};

  // Probe the active CRTC's plane budget and trim the layer set to
  // fit. With layer_count <= usable_planes the bipartite solver has a
  // chance to find a complete placement; the all-or-nothing fallback
  // (assigned=0 composited=N) only triggers when no complete placement
  // exists. A return of 0 here means the registry probe failed — fall
  // through with the full set and let the allocator's report tell the
  // truth.
  const std::size_t usable_planes = count_usable_planes(dev, output->crtc_id);
  const std::size_t layer_count =
      usable_planes == 0 ? layers.size() : std::min(layers.size(), usable_planes);
  drm::println(
      "Plane budget: {} usable plane(s) on CRTC {} (PRIMARY+OVERLAY, cursor excluded). "
      "Building {} layer(s).",
      usable_planes, output->crtc_id, layer_count);

  drm::println("Layer requirements:");
  for (std::size_t i = 0; i < layer_count; ++i) {
    const auto& l = layers.at(i);
    drm::println("  Layer {} format={} ({}) {}", i, format_label(l.format), l.format,
                 l.description);
  }

  const drm::scene::LayerScene::Config cfg{output->crtc_id, output->connector_id, mode};
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  // Tiles laid out non-overlapping in a 2x2 grid covering the centre
  // of the screen. The scaler layer's src_rect is half the size of
  // its dst_rect — that 2× upscale is what makes the scaler match
  // mandatory. The other three layers are 1:1 and don't constrain
  // plane choice on that axis.
  const std::uint32_t tile_w = fb_w / 4U;
  const std::uint32_t tile_h = (fb_h * 6U) / 20U;
  const auto y0 = static_cast<std::int32_t>(fb_h / 5U);

  for (std::size_t i = 0; i < layer_count; ++i) {
    const auto& l = layers.at(i);
    const std::uint32_t buf_w = l.needs_scaling ? (tile_w / 2U) : tile_w;
    const std::uint32_t buf_h = l.needs_scaling ? (tile_h / 2U) : tile_h;

    auto src = drm::scene::DumbBufferSource::create(dev, buf_w, buf_h, l.format);
    if (!src) {
      drm::println(stderr, "DumbBufferSource(layer {} {}): {}", i, format_label(l.format),
                   src.error().message());
      // Format unsupported by the kernel's dumb-buffer path on this
      // driver — log and continue rather than aborting; the example's
      // pedagogy is partly to make this visible.
      continue;
    }
    {
      auto m = src->get()->map(drm::MapAccess::Write);
      if (!m) {
        drm::println(stderr, "map(layer {}): {}", i, m.error().message());
        return EXIT_FAILURE;
      }
      fill_solid(*m, l.c0, l.c1, l.c2, l.c3);
    }

    const auto col = static_cast<std::int32_t>(i % 2U);
    const auto row = static_cast<std::int32_t>(i / 2U);
    drm::scene::LayerDesc desc;
    desc.source = std::move(*src);
    desc.display.src_rect = {0, 0, buf_w, buf_h};
    desc.display.dst_rect = {col * static_cast<std::int32_t>(tile_w),
                             y0 + (row * static_cast<std::int32_t>(tile_h)), tile_w, tile_h};
    desc.display.zpos = 3 + static_cast<int>(i);
    if (auto r = scene->add_layer(std::move(desc)); !r) {
      drm::println(stderr, "add_layer(layer {}): {}", i, r.error().message());
      return EXIT_FAILURE;
    }
  }

  drm::PageFlip page_flip(dev);
  bool flip_pending = false;
  page_flip.set_handler([&](std::uint32_t /*crtc*/, std::uint64_t /*seq*/,
                            std::uint64_t /*ts_ns*/) noexcept { flip_pending = false; });

  // 30 frames steady state — placement is constant across the run
  // because no layer fields change. The bipartite solve happens once
  // on cold start (test_commits > 0) and the warm-start cache holds
  // the assignment for the rest.
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
  }

  while (flip_pending) {
    if (auto r = page_flip.dispatch(-1); !r) {
      drm::println(stderr, "page_flip dispatch (drain): {}", r.error().message());
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
