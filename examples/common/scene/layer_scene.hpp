// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// layer_scene.hpp — top-level scene façade.
//
// Phase 2.1 scope per docs/implementation_plan.md:
//   - Construct from (Device, CRTC/connector/mode).
//   - add/remove/get layers by handle, monotonic + generation.
//   - Single test() / commit() path that runs the existing
//     drm::planes::Allocator against a fresh AtomicRequest.
//   - Diagnostic CommitReport.
//
// Not in Phase 2.1 (shipped in later phases):
//   - Property minimization (Phase 2.2).
//   - Composition fallback for unassigned layers (Phase 2.3).
//   - rebind() for CRTC/connector/mode changes (Phase 2.4).
//   - Page-flip async completion handling / buffer release after scanout.
//
// The pimpl keeps drm::planes::Output, drm::planes::Allocator, and
// drm::PropertyStore out of this header — they are implementation
// details that consumers shouldn't couple to. The forward declaration
// of drmModeModeInfo via <xf86drmMode.h> is intentional; Config takes
// a drmModeModeInfo by value, so the consumer needs the full type.

#pragma once

#include "commit_report.hpp"
#include "layer.hpp"
#include "layer_desc.hpp"
#include "layer_handle.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

class LayerScene {
 public:
  struct Config {
    std::uint32_t crtc_id{0};
    std::uint32_t connector_id{0};
    drmModeModeInfo mode{};
  };

  /// Build a LayerScene bound to the given CRTC + connector + mode.
  /// Enumerates the device's plane registry and caches the connector's
  /// and CRTC's atomic properties up front — neither lookup runs again
  /// for the scene's lifetime (Phase 2.4's rebind() will re-enumerate).
  [[nodiscard]] static drm::expected<std::unique_ptr<LayerScene>, std::error_code> create(
      drm::Device& dev, const Config& cfg);

  ~LayerScene();

  LayerScene(const LayerScene&) = delete;
  LayerScene& operator=(const LayerScene&) = delete;
  LayerScene(LayerScene&&) = delete;
  LayerScene& operator=(LayerScene&&) = delete;

  // ── Layer lifecycle ────────────────────────────────────────────────

  /// Register a new layer. Returns its handle on success. Rejects
  /// std::errc::invalid_argument when desc.source is null.
  [[nodiscard]] drm::expected<LayerHandle, std::error_code> add_layer(LayerDesc desc);

  /// Remove the layer identified by `handle`. Stale handles (removed
  /// layer, slot recycled to a different layer) are a no-op.
  void remove_layer(LayerHandle handle);

  /// Look up a layer by handle. Returns nullptr if the handle doesn't
  /// name a currently-live layer (never allocated, already removed, or
  /// generation-mismatched).
  [[nodiscard]] Layer* get_layer(LayerHandle handle) noexcept;
  [[nodiscard]] const Layer* get_layer(LayerHandle handle) const noexcept;

  [[nodiscard]] std::size_t layer_count() const noexcept;

  // ── Commit cycle ───────────────────────────────────────────────────

  /// Build the frame's AtomicRequest and run it through the allocator
  /// with DRM_MODE_ATOMIC_TEST_ONLY. State is not applied to hardware.
  /// Use to validate a commit shape before committing for real.
  [[nodiscard]] drm::expected<CommitReport, std::error_code> test();

  /// Build and commit this frame. On first commit after create() or
  /// rebind(), DRM_MODE_ATOMIC_ALLOW_MODESET is implicitly added so
  /// the CRTC's MODE_ID / ACTIVE properties can be written.
  ///
  /// `flags` is OR-ed with the implicit flags the scene adds. Callers
  /// who want page-flip events should set DRM_MODE_PAGE_FLIP_EVENT
  /// themselves; the scene does not inject it.
  [[nodiscard]] drm::expected<CommitReport, std::error_code> commit(std::uint32_t flags = 0);

 private:
  class Impl;
  explicit LayerScene(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::scene
