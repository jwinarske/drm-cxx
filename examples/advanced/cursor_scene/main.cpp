// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cursor_scene — present a cursor as an ordinary scene layer via CursorSource.
//
// A full-screen background plus a small arrow cursor that moves in a circle.
// The cursor is a CursorSource (a dumb ARGB sprite the scene places on a plane),
// not the legacy HW-cursor-plane path. On hardware with no CURSOR-type plane
// (e.g. rockchip VOP2) the allocator lands it on an OVERLAY plane; the demo
// prints which plane the cursor took and whether it stayed off the composition
// buffer (composited == 0), which is the property that matters on VOP2.
//
//   ./cursor_scene [/dev/dri/cardN] [frames]

#include "../../common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/cursor_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

// A 32x32 white arrow (lower-left triangle) with a 1px dark outline, hotspot at
// the tip (0,0). Recognizable on any background and trivially scanout-checkable.
constexpr std::uint32_t k_sz = 32;

std::vector<std::uint32_t> make_arrow() {
  std::vector<std::uint32_t> px(static_cast<std::size_t>(k_sz) * k_sz, 0x00000000U);
  for (std::uint32_t y = 0; y < k_sz; ++y) {
    for (std::uint32_t x = 0; x < k_sz; ++x) {
      const bool inside = x <= y && y < 26 && x < 20;
      const bool edge = inside && (x == y || y == 25 || (x == 19 && y >= 19));
      if (edge) {
        px[(y * k_sz) + x] = 0xFF101010U;  // dark outline
      } else if (inside) {
        px[(y * k_sz) + x] = 0xFFFFFFFFU;  // white body
      }
    }
  }
  return px;
}

}  // namespace

int main(int argc, char** argv) try {
  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    drm::println(stderr, "cursor_scene: no usable output");
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const std::int32_t w = output->mode.hdisplay;
  const std::int32_t h = output->mode.vdisplay;
  const int frames = (argc > 2) ? std::atoi(argv[2]) : 300;

  // Background: a flat fill so the cursor is unmistakable.
  auto bg_r = drm::scene::DumbBufferSource::create(
      dev, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), DRM_FORMAT_XRGB8888);
  if (!bg_r) {
    drm::println(stderr, "cursor_scene: bg source: {}", bg_r.error().message());
    return EXIT_FAILURE;
  }
  auto bg = std::move(*bg_r);
  if (auto m = bg->map(drm::MapAccess::Write); m) {
    std::memset(m->pixels().data(), 0x30, m->pixels().size());
  }

  auto cursor_r = drm::scene::CursorSource::create_argb(dev, make_arrow(), k_sz, k_sz, 0, 0);
  if (!cursor_r) {
    drm::println(stderr, "cursor_scene: cursor source: {}", cursor_r.error().message());
    return EXIT_FAILURE;
  }
  auto cursor = std::move(*cursor_r);
  const int hx = cursor->hotspot_x();
  const int hy = cursor->hotspot_y();

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = output->crtc_id;
  cfg.connector_id = output->connector_id;
  cfg.mode = output->mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    drm::println(stderr, "cursor_scene: scene: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  // Background layer (zpos >= 3 to clear the amdgpu PRIMARY pin); cursor above.
  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(bg);
  bg_desc.display.dst_rect = {0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
  bg_desc.display.zpos = 3;
  if (auto r = scene->add_layer(std::move(bg_desc)); !r) {
    drm::println(stderr, "cursor_scene: add bg: {}", r.error().message());
    return EXIT_FAILURE;
  }

  drm::scene::LayerDesc cur_desc;
  cur_desc.source = std::move(cursor);
  cur_desc.display.dst_rect = {0, 0, k_sz, k_sz};
  // Just above the bg. Keep within a conservative zpos range: rockchip VOP2
  // planes cap zpos at 7, so a large value (e.g. 10) is rejected at commit.
  cur_desc.display.zpos = 4;
  auto cur_handle_r = scene->add_layer(std::move(cur_desc));
  if (!cur_handle_r) {
    drm::println(stderr, "cursor_scene: add cursor: {}", cur_handle_r.error().message());
    return EXIT_FAILURE;
  }
  const auto cur_handle = *cur_handle_r;

  drm::println("cursor_scene: {}x{} — arrow cursor as a scene layer over {} frames", w, h, frames);

  bool reported = false;
  for (int f = 0; f < frames; ++f) {
    // Move the cursor around a circle centered on the screen.
    const double t = static_cast<double>(f) * 0.05;
    const auto wd = static_cast<double>(w);
    const auto hd = static_cast<double>(h);
    const auto cx = static_cast<std::int32_t>((wd / 2.0) + (std::cos(t) * (wd / 4.0)));
    const auto cy = static_cast<std::int32_t>((hd / 2.0) + (std::sin(t) * (hd / 4.0)));
    if (auto* layer = scene->get_layer(cur_handle); layer != nullptr) {
      layer->set_dst_rect_if_changed({cx - hx, cy - hy, k_sz, k_sz});
    }

    auto report = scene->commit();
    if (!report) {
      drm::println(stderr, "cursor_scene: commit f={}: {}", f, report.error().message());
      return EXIT_FAILURE;
    }
    if (!reported) {
      reported = true;
      drm::println("cursor_scene: assigned={} composited={}", report->layers_assigned,
                   report->layers_composited);
      for (const auto& p : report->placements) {
        if (p.handle == cur_handle) {
          drm::println(
              "cursor_scene: cursor placement={} plane_id={}",
              p.placement == drm::scene::LayerPlacement::AssignedToPlane ? "plane" : "composited",
              p.plane_id);
        }
      }
    }
    ::usleep(16000);
  }

  drm::println("cursor_scene: done");
  return EXIT_SUCCESS;
} catch (...) {
  return EXIT_FAILURE;
}
