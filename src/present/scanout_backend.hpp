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

namespace drm::present {

class ScanoutProducer;

class ScanoutBackend {
 public:
  struct Config {
    std::uint32_t fourcc{DRM_FORMAT_XRGB8888};
    fmt::Rotation rotation{fmt::Rotation::Rotate0};
  };

  // Discover an output on `dev`, set up a full-screen layer fed by `producer`,
  // and return the ready backend. `producer` must outlive the backend.
  [[nodiscard]] static drm::expected<std::unique_ptr<ScanoutBackend>, std::error_code> create(
      drm::Device& dev, ScanoutProducer& producer, const Config& cfg);

  // Convenience: discover with the default Config (XRGB8888, no rotation).
  [[nodiscard]] static drm::expected<std::unique_ptr<ScanoutBackend>, std::error_code> create(
      drm::Device& dev, ScanoutProducer& producer);

  // Present one frame: commit the scene with `flags` (e.g. DRM_MODE_PAGE_FLIP_EVENT).
  [[nodiscard]] drm::expected<scene::CommitReport, std::error_code> present(
      std::uint32_t flags = 0);

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
};

}  // namespace drm::present
