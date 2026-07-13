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
//   ./damage_present [/dev/dri/cardN] [frames] [--no-seat]

#include "../../common/draw.hpp"
#include "../../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/format.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/present/scanout_format.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <utility>

int main(int argc, char** argv) try {
  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    drm::println(stderr, "damage_present: no usable output");
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const std::int32_t w = output->mode.hdisplay;
  const std::int32_t h = output->mode.vdisplay;

  // Negotiate a format the plane scans out (tilcdc, for one, has no XRGB8888).
  const std::array<std::uint32_t, 2> prefs{DRM_FORMAT_XRGB8888, DRM_FORMAT_RGB565};
  std::uint32_t fourcc = drm::present::negotiate_scanout_format(dev, output->crtc_id, prefs);
  if (fourcc == 0) {
    fourcc = DRM_FORMAT_XRGB8888;
  }

  auto src_r = drm::scene::DumbBufferSource::create(dev, static_cast<std::uint32_t>(w),
                                                    static_cast<std::uint32_t>(h), fourcc);
  if (!src_r) {
    drm::println(stderr, "damage_present: dumb source: {}", src_r.error().message());
    return EXIT_FAILURE;
  }
  auto src = std::move(*src_r);
  auto* dmg = src.get();  // keep a raw handle so we can set_damage after the scene owns it

  constexpr std::uint32_t k_bg = 0x00202020U;  // dark gray
  if (auto m = dmg->map(drm::MapAccess::Write); m) {
    drm::examples::clear(*m, fourcc, k_bg);
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

  drm::println("damage_present: {}x{} {} — moving box, partial updates via FB_DAMAGE_CLIPS", w, h,
               drm::format_name(fourcc));

  const int frames = (argc > 2) ? std::atoi(argv[2]) : 120;
  // Box scaled to ~1/4 of the panel's smaller dimension so it stays a small
  // moving box on tiny displays (e.g. 240x240 SPI panels) rather than a fixed
  // 256px box that overflows the buffer (negative `by`) and segfaults.
  const std::int32_t k_box = static_cast<std::int32_t>(w < h ? w : h) / 4;
  const std::int32_t span_x = (w > k_box) ? (w - k_box) : 1;
  const std::int32_t by = (h - k_box) / 2;
  std::int32_t prev_x = 0;
  for (int f = 0; f < frames; ++f) {
    const std::int32_t bx = (f * 17) % span_x;
    const std::uint32_t color = 0x00FF0000U | static_cast<std::uint32_t>((f * 4) & 0xFF);
    if (auto m = dmg->map(drm::MapAccess::Write); m) {
      drm::examples::fill_rect(*m, fourcc, prev_x, by, k_box, k_box, k_bg);  // erase old
      drm::examples::fill_rect(*m, fourcc, bx, by, k_box, k_box, color);     // draw new
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
} catch (...) {
  return EXIT_FAILURE;
}
