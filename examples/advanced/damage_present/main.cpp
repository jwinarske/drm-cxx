// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// damage_present — validate FB_DAMAGE_CLIPS (frame-economy slice 1).
//
// A full-screen software (dumb-buffer) layer animates a moving box. Each frame
// repaints only the box's previous and new positions (erase + draw) and reports
// exactly those two rectangles as damage via DumbBufferSource::set_damage(). The
// scene turns that into an FB_DAMAGE_CLIPS blob on the plane, so the driver is
// told only those regions changed. On a plane/driver without the property the
// scene silently falls back to a full-frame commit — same pixels, no blob.
//
// Verifies: the partial-update commit is accepted (no EINVAL) and renders
// correctly. The power/bandwidth benefit is an internal driver optimization and
// is not directly observable from userspace.
//
//   ./damage_present [/dev/dri/cardN] [frames]

#include "../../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
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
    drm::println(stderr, "damage_present: no usable output");
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const std::int32_t w = output->mode.hdisplay;
  const std::int32_t h = output->mode.vdisplay;

  auto src_r = drm::scene::DumbBufferSource::create(
      dev, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), DRM_FORMAT_XRGB8888);
  if (!src_r) {
    drm::println(stderr, "damage_present: dumb source: {}", src_r.error().message());
    return EXIT_FAILURE;
  }
  auto src = std::move(*src_r);
  auto* dmg = src.get();  // keep a raw handle so we can set_damage after the scene owns it

  constexpr std::uint32_t k_bg = 0x00202020U;  // dark gray
  if (auto m = dmg->map(drm::MapAccess::Write); m) {
    std::memset(m->pixels().data(), 0x20, m->pixels().size());
  }

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = output->crtc_id;
  cfg.connector_id = output->connector_id;
  cfg.mode = output->mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "damage_present: scene: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  drm::scene::LayerDesc desc;
  desc.source = std::move(src);
  desc.display.dst_rect = {0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
  if (auto r = scene->add_layer(std::move(desc)); !r) {
    drm::println(stderr, "damage_present: add_layer: {}", r.error().message());
    return EXIT_FAILURE;
  }

  drm::println("damage_present: {}x{} — moving box, partial updates via FB_DAMAGE_CLIPS", w, h);

  const int frames = (argc > 2) ? std::atoi(argv[2]) : 120;
  constexpr std::int32_t k_box = 256;
  const std::int32_t span_x = (w > k_box) ? (w - k_box) : 1;
  const std::int32_t by = (h - k_box) / 2;
  std::int32_t prev_x = 0;
  for (int f = 0; f < frames; ++f) {
    const std::int32_t bx = (f * 17) % span_x;
    const std::uint32_t color = 0x00FF0000U | static_cast<std::uint32_t>((f * 4) & 0xFF);
    if (auto m = dmg->map(drm::MapAccess::Write); m) {
      fill_rect(*m, prev_x, by, k_box, k_box, k_bg);  // erase old
      fill_rect(*m, bx, by, k_box, k_box, color);     // draw new
    }
    // Report exactly the two touched regions as this frame's damage.
    const std::array<drm::scene::DamageRect, 2> rects{{
        {prev_x, by, static_cast<std::uint32_t>(k_box), static_cast<std::uint32_t>(k_box)},
        {bx, by, static_cast<std::uint32_t>(k_box), static_cast<std::uint32_t>(k_box)},
    }};
    dmg->set_damage(rects);
    if (auto r = scene->commit(0); !r) {
      drm::println(stderr, "damage_present: commit f={}: {}", f, r.error().message());
      return EXIT_FAILURE;
    }
    prev_x = bx;
    ::usleep(16000);
  }

  drm::println("damage_present: presented {} frames", frames);
  return EXIT_SUCCESS;
}
