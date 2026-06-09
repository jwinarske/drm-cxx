// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// ring_present — validate DumbRingSource + BufferRing (frame-economy slice 2).
//
// A moving box animates through a multi-slot ring (default 3 buffers). Each
// frame the ring leases a slot and tells the app, via the paint callback, the
// stale region that slot must repaint (full for a fresh slot, else the union of
// damage since that slot was last scanned out — the buffer-age contract). The
// app repaints exactly that region + draws the box, and returns what it changed
// this frame; DumbRingSource then reports the repainted union as the buffer's
// damage so the scene emits a correct FB_DAMAGE_CLIPS.
//
// Verifies on HW: the box animates with NO ghosting across slot reuse (proof the
// age-driven repaint union is correct), and the partial-update commit is
// accepted. On a plane/driver without FB_DAMAGE_CLIPS the scene full-frames.
//
//   ./ring_present [/dev/dri/cardN] [frames]

#include "../../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/present/buffer_ring.hpp>
#include <drm-cxx/present/dumb_ring_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

void fill_rect(drm::BufferMapping& m, std::int32_t x, std::int32_t y, std::int32_t w,
               std::int32_t h, std::uint32_t color) {
  const auto px = m.pixels();
  const auto stride = m.stride();
  for (std::int32_t row = y; row < y + h; ++row) {
    std::uint8_t* line =
        px.data() + (static_cast<std::size_t>(row) * stride) + (static_cast<std::size_t>(x) * 4U);
    for (std::int32_t col = 0; col < w; ++col) {
      std::memcpy(line + (static_cast<std::size_t>(col) * 4U), &color, 4U);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    drm::println(stderr, "ring_present: no usable output");
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const std::int32_t w = output->mode.hdisplay;
  const std::int32_t h = output->mode.vdisplay;

  auto src_r = drm::present::DumbRingSource::create(
      dev, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), DRM_FORMAT_XRGB8888, 3);
  if (!src_r) {
    drm::println(stderr, "ring_present: DumbRingSource: {}", src_r.error().message());
    return EXIT_FAILURE;
  }
  auto src = std::move(*src_r);
  auto* ring = src.get();  // raw handle: paint() each frame, scene owns the source

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = output->crtc_id;
  cfg.connector_id = output->connector_id;
  cfg.mode = output->mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "ring_present: scene: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  drm::scene::LayerDesc desc;
  desc.source = std::move(src);
  desc.display.dst_rect = {0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
  if (auto r = scene->add_layer(std::move(desc)); !r) {
    drm::println(stderr, "ring_present: add_layer: {}", r.error().message());
    return EXIT_FAILURE;
  }

  drm::println("ring_present: {}x{} — 3-slot ring, buffer-age repaint via FB_DAMAGE_CLIPS", w, h);

  constexpr std::uint32_t k_bg = 0x00202020U;
  constexpr std::int32_t k_box = 256;
  const std::int32_t span_x = (w > k_box) ? (w - k_box) : 1;
  const std::int32_t by = (h - k_box) / 2;
  const int frames = (argc > 2) ? std::atoi(argv[2]) : 120;

  std::int32_t prev_x = 0;
  bool have_prev = false;
  for (int f = 0; f < frames; ++f) {
    const std::int32_t bx = (f * 17) % span_x;
    const std::uint32_t color = 0x0000FF00U | static_cast<std::uint32_t>((f * 4) & 0xFF);

    auto pr = ring->paint([&](drm::BufferMapping& m, const drm::present::Repaint& rp) {
      if (rp.full) {
        std::memset(m.pixels().data(), 0x20, m.pixels().size());  // whole buffer
      } else {
        for (const drm::present::Rect& r : rp.region) {  // erase the slot's stale union
          fill_rect(m, r.x, r.y, static_cast<std::int32_t>(r.width),
                    static_cast<std::int32_t>(r.height), k_bg);
        }
      }
      fill_rect(m, bx, by, k_box, k_box, color);  // draw the box

      // What changed this frame vs the previous frame: the box's old + new spots.
      std::vector<drm::present::Rect> fd;
      fd.push_back({bx, by, static_cast<std::uint32_t>(k_box), static_cast<std::uint32_t>(k_box)});
      if (have_prev) {
        fd.push_back(
            {prev_x, by, static_cast<std::uint32_t>(k_box), static_cast<std::uint32_t>(k_box)});
      }
      return fd;
    });
    if (!pr) {
      // Ring momentarily out of free slots — retry next vblank.
      ::usleep(16000);
      continue;
    }
    prev_x = bx;
    have_prev = true;

    if (auto r = scene->commit(0); !r) {
      drm::println(stderr, "ring_present: commit f={}: {}", f, r.error().message());
      return EXIT_FAILURE;
    }
    ::usleep(16000);
  }

  drm::println("ring_present: presented {} frames", frames);
  return EXIT_SUCCESS;
}
