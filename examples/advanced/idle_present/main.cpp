// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// idle_present — validate FrameEconomy idle-suppression (frame-economy slice 3).
//
// A single software layer changes content only every Nth frame; the rest are
// idle. The loop runs every frame through FrameEconomy::decide(): unchanged
// frames return Skip and issue NO atomic commit (no page flip, no scanout
// reprogram — the power win, driver-agnostic and not gated on PSR). Changed
// frames commit, damaged when the producer supplied damage.
//
// Measurable: prints committed-vs-skipped counts. With a change every 30 frames
// over 300, expect ~10 commits and ~290 skips — i.e. ~97% of page flips avoided
// while idle. The display still updates correctly on the changed frames.
//
//   ./idle_present [/dev/dri/cardN] [frames] [change_period]

#include "../../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/present/frame_economy.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <utility>

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
    drm::println(stderr, "idle_present: no usable output");
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const std::int32_t w = output->mode.hdisplay;
  const std::int32_t h = output->mode.vdisplay;

  auto src_r = drm::scene::DumbBufferSource::create(
      dev, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), DRM_FORMAT_XRGB8888);
  if (!src_r) {
    drm::println(stderr, "idle_present: dumb source: {}", src_r.error().message());
    return EXIT_FAILURE;
  }
  auto src = std::move(*src_r);
  auto* dmg = src.get();
  if (auto m = dmg->map(drm::MapAccess::Write); m) {
    std::memset(m->pixels().data(), 0x20, m->pixels().size());
  }

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = output->crtc_id;
  cfg.connector_id = output->connector_id;
  cfg.mode = output->mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "idle_present: scene: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  drm::scene::LayerDesc desc;
  desc.source = std::move(src);
  desc.display.dst_rect = {0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
  if (auto r = scene->add_layer(std::move(desc)); !r) {
    drm::println(stderr, "idle_present: add_layer: {}", r.error().message());
    return EXIT_FAILURE;
  }

  const int frames = (argc > 2) ? std::atoi(argv[2]) : 300;
  const int period = (argc > 3) ? std::atoi(argv[3]) : 30;
  drm::println("idle_present: {}x{} — change every {} frames over {}", w, h, period, frames);

  constexpr std::int32_t k_box = 256;
  const std::int32_t bx = (w - k_box) / 2;
  const std::int32_t by = (h - k_box) / 2;
  drm::present::FrameEconomy econ;

  for (int f = 0; f < frames; ++f) {
    const bool changed = (f % period == 0);
    bool damage_available = false;
    if (changed) {
      const std::uint32_t color = 0x00FF0000U | static_cast<std::uint32_t>((f * 8) & 0xFF);
      if (auto m = dmg->map(drm::MapAccess::Write); m) {
        fill_rect(*m, bx, by, k_box, k_box, color);
      }
      const std::array<drm::scene::DamageRect, 1> rects{
          {{bx, by, static_cast<std::uint32_t>(k_box), static_cast<std::uint32_t>(k_box)}}};
      dmg->set_damage(rects);
      damage_available = true;
    }

    const auto decision = econ.decide(changed, damage_available);
    if (decision.action == drm::present::FrameAction::Skip) {
      ::usleep(16000);  // idle: no commit, no flip
      continue;
    }
    if (auto r = scene->commit(0); !r) {
      drm::println(stderr, "idle_present: commit f={}: {}", f, r.error().message());
      return EXIT_FAILURE;
    }
    ::usleep(16000);
  }

  drm::println("idle_present: {} frames — committed {}, skipped {} ({}% flips avoided)", frames,
               econ.committed(), econ.skipped(),
               frames > 0 ? (econ.skipped() * 100 / static_cast<std::uint64_t>(frames)) : 0);
  return EXIT_SUCCESS;
}
