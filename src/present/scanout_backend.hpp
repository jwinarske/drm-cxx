// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// present/scanout_backend.hpp
//
// Ties the discovery / negotiation / producer / scene pieces into one
// full-screen presenter. create() discovers the output, probes the driver,
// negotiates the producer's modifiers against the plane, allocates a buffer, and
// builds a single-layer LayerScene; present() drives one atomic commit.
// LayerScene owns the plane allocator, composition fallback, deferred release,
// and session pause/resume underneath -- the backend is just the wiring above it.

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/driver_profile.hpp>
#include <drm-cxx/display/scanout_target.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/present/frame_economy.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/layer_handle.hpp>

#include <drm_fourcc.h>

#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}
namespace drm::scene {
class LayerScene;
}
namespace drm::sync {
class SyncFence;
}

namespace drm::present {

class ScanoutProducer;

class ScanoutBackend {
 public:
  // Variable-refresh-rate policy for the discovered CRTC. VRR lets the panel
  // track the actual flip cadence, which pairs with the idle-Skip (flips stop
  // when content is static). Off by default because VRR can cause visible
  // brightness flicker on some panels, so it stays opt-in.
  enum class VrrPolicy : std::uint8_t {
    Off,   // never arm VRR_ENABLED (default)
    Auto,  // arm VRR_ENABLED iff the driver profile reports vrr_capable
    On,    // request VRR_ENABLED (a no-op on CRTCs that don't expose it)
  };

  struct Config {
    std::uint32_t fourcc{DRM_FORMAT_XRGB8888};
    fmt::Rotation rotation{fmt::Rotation::Rotate0};
    VrrPolicy vrr{VrrPolicy::Off};
  };

  // Discover an output on `dev`, set up a full-screen layer fed by `producer`,
  // and return the ready backend. `producer` must outlive the backend.
  [[nodiscard]] static drm::expected<std::unique_ptr<ScanoutBackend>, std::error_code> create(
      drm::Device& dev, ScanoutProducer& producer, const Config& cfg);

  // Convenience: discover with the default Config (XRGB8888, no rotation).
  [[nodiscard]] static drm::expected<std::unique_ptr<ScanoutBackend>, std::error_code> create(
      drm::Device& dev, ScanoutProducer& producer);

  // Present one frame: commit the scene with `flags` (e.g. DRM_MODE_PAGE_FLIP_EVENT).
  // `out_fence` (opt-in) receives a sync_file that signals when the frame is
  // scanned out — wait on it before reusing the buffer the producer just rendered.
  [[nodiscard]] drm::expected<scene::CommitReport, std::error_code> present(
      std::uint32_t flags = 0, drm::sync::SyncFence* out_fence = nullptr);

  // Present consulting the backend's FrameEconomy. When `content_changed` is
  // false and this is not the first frame, NO atomic commit is issued (the
  // idle-Skip: no page flip, no scanout reprogram — a power win on every panel,
  // and it lets a PSR-capable panel stay in self-refresh) and the returned
  // report has `skipped_idle == true` with every other field zero. Otherwise it
  // commits exactly like present(): the scene already emits FB_DAMAGE_CLIPS iff
  // the producer reported bounded per-frame damage, so damaged-vs-full needs no
  // extra signal here. The first call after create() always commits (the
  // scanout buffer is otherwise undefined); call force_full_present() after a
  // mode change / rebind to force the next frame to commit regardless.
  [[nodiscard]] drm::expected<scene::CommitReport, std::error_code> present_if_changed(
      bool content_changed, std::uint32_t flags = 0, drm::sync::SyncFence* out_fence = nullptr);

  // Force the next present_if_changed() to commit regardless of content_changed
  // — call after a modeset / mode change / rebind, where the previous scanout
  // contents no longer apply.
  void force_full_present() noexcept { economy_.force_full(); }

  // FrameEconomy counters (committed vs idle-skipped frames) for telemetry and
  // tests. Both are 0 until the first present_if_changed().
  [[nodiscard]] std::uint64_t frames_committed() const noexcept { return economy_.committed(); }
  [[nodiscard]] std::uint64_t frames_skipped() const noexcept { return economy_.skipped(); }

  // Arm or disarm the CRTC's VRR_ENABLED at runtime (e.g. enable during video /
  // animation, disable when static). Forwards to LayerScene::set_vrr_enabled and
  // takes effect on the next present(); a no-op on CRTCs without VRR_ENABLED (see
  // vrr_capable()).
  void set_vrr(bool enable);

  // Whether the discovered CRTC advertises VRR_ENABLED (VRR is drivable here).
  [[nodiscard]] bool vrr_capable() const noexcept { return profile_.vrr_capable; }

  [[nodiscard]] const display::ScanoutTarget& target() const noexcept { return target_; }
  [[nodiscard]] const display::DriverProfile& profile() const noexcept { return profile_; }
  [[nodiscard]] scene::LayerScene& scene() noexcept { return *scene_; }
  // The negotiated modifier set, most-preferred first; empty when the backend
  // fell back to LINEAR (no IN_FORMATS or no overlap).
  [[nodiscard]] drm::span<const fmt::Modifier> modifiers() const noexcept {
    return {modifiers_.data(), modifiers_.size()};
  }

  ScanoutBackend(const ScanoutBackend&) = delete;
  ScanoutBackend& operator=(const ScanoutBackend&) = delete;
  ScanoutBackend(ScanoutBackend&&) = delete;
  ScanoutBackend& operator=(ScanoutBackend&&) = delete;
  ~ScanoutBackend();

 private:
  ScanoutBackend(display::ScanoutTarget target, display::DriverProfile profile,
                 std::vector<fmt::Modifier> modifiers, std::unique_ptr<scene::LayerScene> scene,
                 scene::LayerHandle layer) noexcept;

  display::ScanoutTarget target_;
  display::DriverProfile profile_;
  std::vector<fmt::Modifier> modifiers_;
  std::unique_ptr<scene::LayerScene> scene_;
  scene::LayerHandle layer_;
  FrameEconomy economy_;
};

}  // namespace drm::present
