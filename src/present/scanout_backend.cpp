// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/scanout_backend.cpp

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/display/driver_profile.hpp>
#include <drm-cxx/display/scanout_target.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/present/frame_economy.hpp>
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
#include <xf86drmMode.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::present {

ScanoutBackend::ScanoutBackend(display::ScanoutTarget target, display::DriverProfile profile,
                               std::vector<fmt::Modifier> modifiers,
                               std::unique_ptr<scene::LayerScene> scene, scene::LayerHandle layer,
                               int fd, RestorePolicy restore,
                               std::optional<SavedCrtc> saved) noexcept
    : target_(std::move(target)),
      profile_(std::move(profile)),
      modifiers_(std::move(modifiers)),
      scene_(std::move(scene)),
      layer_(layer),
      fd_(fd),
      restore_(restore),
      saved_crtc_(saved) {}

ScanoutBackend::~ScanoutBackend() {
  // Release the scene first: it drains the deferred-release ring back to the
  // producer and destroys the scene's framebuffers, so the CRTC restore below
  // reprograms a pipeline that no longer references our buffers. (Callers must
  // have already landed any in-flight page-flip event — see LayerScene::drain.)
  scene_.reset();

  if (restore_ != RestorePolicy::SavedCrtc || !saved_crtc_.has_value() || fd_ < 0) {
    return;
  }
  // Re-apply the CRTC state captured at create(). A saved buffer_id of 0 means
  // the CRTC was inactive before we took it over, so disable it (no connectors,
  // no mode) rather than re-lighting a framebuffer that never existed. Legacy
  // drmModeSetCrtc, as the reference DRM drivers use for restore. Best-effort:
  // the fd may already be gone at teardown, and a failed restore must not throw.
  const SavedCrtc& s = *saved_crtc_;
  const bool had_fb = s.buffer_id != 0;
  drmModeModeInfo mode = s.mode;
  std::uint32_t connector = target_.connector_id;
  (void)drmModeSetCrtc(fd_, target_.crtc_id, s.buffer_id, s.x, s.y, had_fb ? &connector : nullptr,
                       had_fb ? 1 : 0, (had_fb && s.mode_valid) ? &mode : nullptr);
}

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

  // Snapshot the CRTC now — before the scene ever commits — so
  // RestorePolicy::SavedCrtc can put it back at teardown. Captured whether or
  // not it is active; a disabled CRTC yields buffer_id 0, which the destructor
  // restores as "leave off". Failure to read it just skips restore.
  std::optional<SavedCrtc> saved;
  if (cfg.restore == RestorePolicy::SavedCrtc) {
    if (drmModeCrtcPtr c = drmModeGetCrtc(dev.fd(), target->crtc_id); c != nullptr) {
      saved = SavedCrtc{c->buffer_id, c->x, c->y, c->mode, c->mode_valid != 0};
      drmModeFreeCrtc(c);
    }
  }

  // Build the scene first so we can negotiate the producer's modifiers against
  // the union across ALL non-cursor planes (candidate_modifiers), not just the
  // primary's IN_FORMATS. On split SoCs (e.g. RK3588/VOP2) the primary plane
  // advertises only AFBC while the GPU exports LINEAR — negotiating primary-only
  // would intersect to empty and miss the LINEAR-capable overlay the allocator
  // will actually place the layer on. The atomic TEST during placement remains
  // the real arbiter of which plane accepts the chosen modifier.
  scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = target->crtc_id;
  scene_cfg.connector_id = target->connector_id;
  scene_cfg.mode = target->mode;
  scene_cfg.stream_capability = scene::stream_capability_unsupported();
  auto scene = scene::LayerScene::create(dev, scene_cfg);
  if (!scene) {
    return drm::unexpected<std::error_code>(scene.error());
  }

  std::vector<fmt::Modifier> producer_mods;
  for (const std::uint64_t value : producer.exportable_modifiers(cfg.fourcc)) {
    producer_mods.push_back(fmt::Modifier{value});
  }
  std::vector<fmt::Modifier> plane_mods;
  for (const std::uint64_t value : (*scene)->candidate_modifiers(cfg.fourcc)) {
    plane_mods.push_back(fmt::Modifier{value});
  }
  // No overlap (or no IN_FORMATS) -> fall back to LINEAR, which every plane can
  // scan out; the allocator's TEST_ONLY then decides where it lands.
  std::vector<fmt::Modifier> negotiated;
  if (!plane_mods.empty()) {
    negotiated = negotiate(producer_mods, plane_mods, cfg.rotation);
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

  scene::LayerDesc desc;
  desc.source = std::move(*source);
  desc.display.dst_rect = {0, 0, target->mode.hdisplay, target->mode.vdisplay};
  // src_rect left {0,0,0,0} -> the whole buffer.
  auto layer = (*scene)->add_layer(std::move(desc));
  if (!layer) {
    return drm::unexpected<std::error_code>(layer.error());
  }

  // Arm VRR from the driver profile per the caller's policy. On (unconditional)
  // and Auto (only where the profile reports the capability) both drive the
  // scene's VRR_ENABLED; the scene silently swallows it on CRTCs that lack the
  // property, so On is safe even off a non-VRR target.
  if (cfg.vrr == VrrPolicy::On || (cfg.vrr == VrrPolicy::Auto && profile->vrr_capable)) {
    (*scene)->set_vrr_enabled(true);
  }

  return std::unique_ptr<ScanoutBackend>(
      new ScanoutBackend(std::move(*target), std::move(*profile), std::move(negotiated),
                         std::move(*scene), *layer, dev.fd(), cfg.restore, saved));
}

drm::expected<std::unique_ptr<ScanoutBackend>, std::error_code> ScanoutBackend::create(
    drm::Device& dev, ScanoutProducer& producer) {
  return create(dev, producer, Config{});
}

void ScanoutBackend::set_vrr(bool enable) {
  scene_->set_vrr_enabled(enable);
}

drm::expected<scene::CommitReport, std::error_code> ScanoutBackend::present(
    std::uint32_t flags, drm::sync::SyncFence* out_fence) {
  return scene_->commit(flags, nullptr, out_fence);
}

drm::expected<scene::CommitReport, std::error_code> ScanoutBackend::present_if_changed(
    bool content_changed, std::uint32_t flags, drm::sync::SyncFence* out_fence) {
  // Damaged-vs-full is decided by the scene from the producer's per-frame damage
  // report (arm_layer_damage_clips), so the economy only owns the Skip here; the
  // `full` field of the decision is intentionally unused.
  const FrameDecision decision = economy_.decide(content_changed, /*damage_available=*/false);
  if (decision.action == FrameAction::Skip) {
    scene::CommitReport report;
    report.skipped_idle = true;
    return report;
  }
  return present(flags, out_fence);
}

}  // namespace drm::present
