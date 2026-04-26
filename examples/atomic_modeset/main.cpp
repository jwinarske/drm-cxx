// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// atomic_modeset — minimal end-to-end LayerScene "hello world".
//
// Opens a DRM device (through libseat when available), picks the first
// connected connector + its CRTC + preferred mode, allocates a single
// full-screen XRGB8888 layer painted with a horizontal black-to-white
// gradient, commits it via drm::scene::LayerScene, and exits after the
// first page-flip event arrives.
//
// Usage: atomic_modeset [/dev/dri/cardN]

#include "../common/select_device.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/mode.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>

int main(int argc, char* argv[]) {
  const auto path = drm::examples::select_device(argc, argv);
  if (!path) {
    return EXIT_FAILURE;
  }

  // Prefer the seat's revocable fd when libseat is available — it owns
  // master and resolves the open-vs-set_master race for us. Plain
  // drm::Device::open is the fallback for stacks without seatd/logind.
  auto seat = drm::session::Seat::open();
  const auto seat_dev = seat ? seat->take_device(*path) : std::nullopt;
  auto dev_holder = [&]() -> std::optional<drm::Device> {
    if (seat_dev) {
      return drm::Device::from_fd(seat_dev->fd);
    }
    auto r = drm::Device::open(*path);
    if (!r) {
      return std::nullopt;
    }
    return std::move(*r);
  }();
  if (!dev_holder) {
    drm::println(stderr, "Failed to open {}", *path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_holder;

  if (auto r = dev.enable_universal_planes(); !r) {
    drm::println(stderr, "enable_universal_planes: {}", r.error().message());
    return EXIT_FAILURE;
  }
  if (auto r = dev.enable_atomic(); !r) {
    drm::println(stderr, "enable_atomic: {}", r.error().message());
    return EXIT_FAILURE;
  }

  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    drm::println(stderr, "get_resources failed");
    return EXIT_FAILURE;
  }
  drm::Connector conn{nullptr, &drmModeFreeConnector};
  const auto connector_ids = drm::span<const std::uint32_t>(res->connectors, res->count_connectors);
  for (const auto cid : connector_ids) {
    if (auto c = drm::get_connector(dev.fd(), cid);
        c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 && c->encoder_id != 0) {
      conn = std::move(c);
      break;
    }
  }
  if (!conn) {
    drm::println(stderr, "No connected connector with an attached encoder");
    return EXIT_FAILURE;
  }
  const auto enc = drm::get_encoder(dev.fd(), conn->encoder_id);
  if (!enc || enc->crtc_id == 0) {
    drm::println(stderr, "No encoder/CRTC for connector {}", conn->connector_id);
    return EXIT_FAILURE;
  }
  const auto mode_res =
      drm::select_preferred_mode(drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes));
  if (!mode_res) {
    drm::println(stderr, "No usable mode on connector {}", conn->connector_id);
    return EXIT_FAILURE;
  }
  const auto mode = *mode_res;
  const std::uint32_t fb_w = mode.width();
  const std::uint32_t fb_h = mode.height();
  drm::println("Modeset: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h, mode.refresh(),
               conn->connector_id, enc->crtc_id);

  // Single full-screen XRGB8888 layer. Paint a horizontal black-to-white
  // gradient: proves the framebuffer reached the screen and that the
  // byte order is right (a wrong-channel write would tint the gradient).
  auto bg_src = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src) {
    drm::println(stderr, "DumbBufferSource::create: {}", bg_src.error().message());
    return EXIT_FAILURE;
  }
  auto* bg = bg_src->get();
  {
    const auto pixels = bg->pixels();
    const auto stride = bg->stride();
    for (std::uint32_t y = 0; y < fb_h; ++y) {
      auto* row = pixels.data() + (static_cast<std::size_t>(y) * stride);
      for (std::uint32_t x = 0; x < fb_w; ++x) {
        const auto v = static_cast<std::uint8_t>((x * 255U) / (fb_w - 1U));
        row[(x * 4U) + 0U] = v;  // B
        row[(x * 4U) + 1U] = v;  // G
        row[(x * 4U) + 2U] = v;  // R
        row[(x * 4U) + 3U] = 0;  // X
      }
    }
  }

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = enc->crtc_id;
  cfg.connector_id = conn->connector_id;
  cfg.mode = mode.drm_mode;
  auto scene_res = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_res) {
    drm::println(stderr, "LayerScene::create: {}", scene_res.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_res);

  drm::scene::LayerDesc desc;
  desc.source = std::move(*bg_src);
  desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  if (auto r = scene->add_layer(std::move(desc)); !r) {
    drm::println(stderr, "add_layer: {}", r.error().message());
    return EXIT_FAILURE;
  }

  drm::PageFlip page_flip(dev);
  bool flipped = false;
  page_flip.set_handler([&](std::uint32_t crtc, std::uint64_t seq, std::uint64_t ts_ns) {
    drm::println("Page flip on CRTC {}: seq={} timestamp={}ns", crtc, seq, ts_ns);
    flipped = true;
  });

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "commit: {}", r.error().message());
    return EXIT_FAILURE;
  }
  while (!flipped) {
    if (auto r = page_flip.dispatch(-1); !r) {
      drm::println(stderr, "page_flip dispatch: {}", r.error().message());
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
