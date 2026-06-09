// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/scanout_backend.cpp

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/display/driver_profile.hpp>
#include <drm-cxx/display/scanout_target.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/present/negotiate.hpp>
#include <drm-cxx/present/scanout_backend.hpp>
#include <drm-cxx/present/scanout_producer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/stream_capability.hpp>

#include <drm_fourcc.h>

#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::present {

ScanoutBackend::ScanoutBackend(display::ScanoutTarget target, display::DriverProfile profile,
                               std::vector<fmt::Modifier> modifiers,
                               std::unique_ptr<scene::LayerScene> scene,
                               scene::LayerHandle layer) noexcept
    : target_(std::move(target)),
      profile_(std::move(profile)),
      modifiers_(std::move(modifiers)),
      scene_(std::move(scene)),
      layer_(layer) {}

ScanoutBackend::~ScanoutBackend() = default;

drm::expected<std::unique_ptr<ScanoutBackend>, std::error_code> ScanoutBackend::create(
    drm::Device& dev, ScanoutProducer& producer, const Config& cfg) {
  // Enable the atomic + universal-planes caps up front: MODE_ID / ACTIVE and the
  // non-primary planes only become visible once these are set, and LayerScene
  // caches the CRTC/connector/plane properties at create() time.
  (void)dev.enable_universal_planes();
  (void)dev.enable_atomic();

  auto target = display::ScanoutTarget::discover(dev);
  if (!target) {
    return drm::unexpected<std::error_code>(target.error());
  }
  auto profile = display::DriverProfile::probe(dev);
  if (!profile) {
    return drm::unexpected<std::error_code>(profile.error());
  }

  // Negotiate the producer's modifiers against the plane's IN_FORMATS (when the
  // driver exposes them). No blob or no overlap -> fall back to LINEAR, which
  // every plane can scan out.
  std::vector<fmt::Modifier> producer_mods;
  for (const std::uint64_t value : producer.exportable_modifiers(cfg.fourcc)) {
    producer_mods.push_back(fmt::Modifier{value});
  }
  std::vector<fmt::Modifier> negotiated;
  if (const auto& plane_formats = target->primary_formats; plane_formats.has_value()) {
    negotiated = negotiate(producer_mods, *plane_formats, cfg.fourcc, cfg.rotation);
  }
  std::vector<std::uint64_t> allowed;
  if (negotiated.empty()) {
    allowed.push_back(DRM_FORMAT_MOD_LINEAR);
  } else {
    for (const fmt::Modifier mod : negotiated) {
      allowed.push_back(mod.value);
    }
  }

  auto source =
      producer.create_buffer(target->mode.hdisplay, target->mode.vdisplay, cfg.fourcc, allowed);
  if (!source) {
    return drm::unexpected<std::error_code>(source.error());
  }

  scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = target->crtc_id;
  scene_cfg.connector_id = target->connector_id;
  scene_cfg.mode = target->mode;
  scene_cfg.stream_capability = scene::stream_capability_unsupported();
  auto scene = scene::LayerScene::create(dev, scene_cfg);
  if (!scene) {
    return drm::unexpected<std::error_code>(scene.error());
  }

  scene::LayerDesc desc;
  desc.source = std::move(*source);
  desc.display.dst_rect = {0, 0, target->mode.hdisplay, target->mode.vdisplay};
  // src_rect left {0,0,0,0} -> the whole buffer.
  auto layer = (*scene)->add_layer(std::move(desc));
  if (!layer) {
    return drm::unexpected<std::error_code>(layer.error());
  }

  return std::unique_ptr<ScanoutBackend>(new ScanoutBackend(
      std::move(*target), std::move(*profile), std::move(negotiated), std::move(*scene), *layer));
}

drm::expected<std::unique_ptr<ScanoutBackend>, std::error_code> ScanoutBackend::create(
    drm::Device& dev, ScanoutProducer& producer) {
  return create(dev, producer, Config{});
}

drm::expected<scene::CommitReport, std::error_code> ScanoutBackend::present(
    std::uint32_t flags, drm::sync::SyncFence* out_fence) {
  return scene_->commit(flags, nullptr, out_fence);
}

}  // namespace drm::present
