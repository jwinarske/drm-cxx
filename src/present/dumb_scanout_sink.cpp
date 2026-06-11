// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "dumb_scanout_sink.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/present/buffer_ring.hpp>
#include <drm-cxx/present/dumb_ring_source.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::present {

DumbScanoutSink::DumbScanoutSink(std::unique_ptr<scene::LayerScene> scene, DumbRingSource* ring,
                                 std::uint32_t w, std::uint32_t h, std::uint32_t refresh) noexcept
    : scene_(std::move(scene)), ring_(ring), width_(w), height_(h), refresh_(refresh) {}

DumbScanoutSink::~DumbScanoutSink() = default;

drm::expected<std::unique_ptr<DumbScanoutSink>, std::error_code> DumbScanoutSink::create(
    drm::Device& dev, std::uint32_t crtc_id, std::uint32_t connector_id,
    const drmModeModeInfo& mode, const Config& cfg) {
  const std::uint32_t format = cfg.drm_format != 0 ? cfg.drm_format : DRM_FORMAT_XRGB8888;
  const auto width = static_cast<std::uint32_t>(mode.hdisplay);
  const auto height = static_cast<std::uint32_t>(mode.vdisplay);
  const std::size_t slots = cfg.buffers != 0 ? cfg.buffers : 2;

  auto src_r = DumbRingSource::create(dev, width, height, format, slots);
  if (!src_r) {
    return drm::unexpected<std::error_code>(src_r.error());
  }
  auto src = std::move(*src_r);
  auto* ring = src.get();

  scene::LayerScene::Config scfg;
  scfg.crtc_id = crtc_id;
  scfg.connector_id = connector_id;
  scfg.mode = mode;
  auto scene_r = scene::LayerScene::create(dev, scfg);
  if (!scene_r) {
    return drm::unexpected<std::error_code>(scene_r.error());
  }
  auto scene = std::move(*scene_r);

  scene::LayerDesc desc;
  desc.source = std::move(src);
  desc.display.dst_rect = {0, 0, width, height};
  if (auto r = scene->add_layer(std::move(desc)); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }

  return std::unique_ptr<DumbScanoutSink>(
      new DumbScanoutSink(std::move(scene), ring, width, height, mode.vrefresh));
}

drm::expected<std::unique_ptr<DumbScanoutSink>, std::error_code> DumbScanoutSink::create(
    drm::Device& dev, std::uint32_t crtc_id, std::uint32_t connector_id,
    const drmModeModeInfo& mode) {
  return create(dev, crtc_id, connector_id, mode, Config{});
}

drm::expected<scene::CommitReport, std::error_code> DumbScanoutSink::present(
    drm::span<const std::byte> src, std::uint32_t src_stride, std::uint32_t flags,
    drm::PageFlip* flip) {
  if (src.size() < static_cast<std::size_t>(height_) * src_stride) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  auto painted = ring_->paint([&](drm::BufferMapping& m, const Repaint& /*repaint*/) {
    const auto dst = m.pixels();
    const auto dst_stride = m.stride();
    const auto row_bytes = std::min<std::size_t>(src_stride, dst_stride);
    for (std::uint32_t y = 0; y < height_; ++y) {
      std::memcpy(dst.data() + (static_cast<std::size_t>(y) * dst_stride),
                  src.data() + (static_cast<std::size_t>(y) * src_stride), row_bytes);
    }
    // A finished CPU frame is a full repaint.
    return std::vector<Rect>{Rect{0, 0, width_, height_}};
  });
  if (!painted) {
    return drm::unexpected<std::error_code>(painted.error());
  }
  return scene_->commit(flags, flip);
}

}  // namespace drm::present
