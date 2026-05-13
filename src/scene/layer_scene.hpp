// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// layer_scene.hpp — top-level scene façade.
//
// scope per docs/implementation_plan.md:
//   - Construct from (Device, CRTC/connector/mode).
//   - add/remove/get layers by handle, monotonic + generation.
//   - Single test() / commit() path that runs the existing
//     drm::planes::Allocator against a fresh AtomicRequest.
//   - Diagnostic CommitReport.
//
// Not in (shipped in later phases):
//   - Property minimization.
//   - Composition fallback for unassigned layers.
//   - rebind() for CRTC/connector/mode changes.
//   - Page-flip async completion handling / buffer release after scanout.
//
// The pimpl keeps drm::planes::Output, drm::planes::Allocator, and
// drm::PropertyStore out of this header — they are implementation
// details that consumers shouldn't couple to. The forward declaration
// of drmModeModeInfo via <xf86drmMode.h> is intentional; Config takes
// a drmModeModeInfo by value, so the consumer needs the full type.

#pragma once

#include "commit_report.hpp"
#include "compatibility_report.hpp"
#include "layer.hpp"
#include "layer_desc.hpp"
#include "layer_handle.hpp"
#include "stream_capability.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::display {
struct HdrSourceMetadata;
}  // namespace drm::display

namespace drm::scene {

class LayerScene {
 public:
  struct Config {
    std::uint32_t crtc_id{0};
    std::uint32_t connector_id{0};
    drmModeModeInfo mode{};

    /// EGL Streams capability for this scene (M7). Defaults to
    /// `StreamMixingMode::Unsupported` so `EglStreamSource` layers
    /// are rejected at `add_layer` time. Callers who need streams
    /// must call `probe_stream_capability(dev)` and assign the
    /// result before passing this Config to `create()`; tests may
    /// construct a `StreamCapability` directly.
    ///
    /// Storing the capability on the Config (rather than auto-probing
    /// inside `create()`) keeps construction free of implicit IO on
    /// every scene — the dlopen-probe runs only when the application
    /// actually intends to use streams.
    StreamCapability stream_capability{};
  };

  /// Build a LayerScene bound to the given CRTC + connector + mode.
  /// Enumerates the device's plane registry and caches the connector's
  /// and CRTC's atomic properties up front — neither lookup runs again
  /// for the scene's lifetime will re-enumerate).
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

  /// The stream capability the scene was constructed with. Callers
  /// inspect this to decide whether to add `EglStreamSource`-backed
  /// layers (when `mixing != StreamMixingMode::Unsupported`) and to
  /// branch the producer-side wiring on the extension set the driver
  /// exposes. Survives `rebind()` and pause/resume verbatim — the
  /// capability describes the driver, not the connector or CRTC.
  [[nodiscard]] const StreamCapability& stream_capability() const noexcept;

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
  /// themselves; the scene does not inject it. `user_data` is forwarded
  /// verbatim to drmModeAtomicCommit — the kernel routes it back as the
  /// user_data arg of page_flip_handler{,2} when the flip completes, so
  /// callers typically pass their PageFlip* here.
  [[nodiscard]] drm::expected<CommitReport, std::error_code> commit(std::uint32_t flags = 0,
                                                                    void* user_data = nullptr);

  /// Set or clear the HDR static metadata signaled on this scene's
  /// connector. Wires the per-CRTC `HdrMetadataCache` so the next
  /// `commit()` writes the connector's `HDR_OUTPUT_METADATA`
  /// property to the corresponding blob id (or 0 to clear).
  ///
  /// `nullopt` clears HDR signaling. The connector returns to SDR
  /// (the kernel emits no HDR Static Metadata InfoFrame).
  ///
  /// Connectors that don't expose `HDR_OUTPUT_METADATA` (older
  /// kernels, non-HDR sinks) silently swallow the call: no kernel
  /// state changes, no error. Use
  /// `drm::display::probe_connector_capabilities` if you want to
  /// gate up front.
  ///
  /// Calling with the same metadata on consecutive commits is a
  /// no-op at the kernel level — the cache hashes content and
  /// returns the existing blob id.
  void set_output_metadata(const std::optional<drm::display::HdrSourceMetadata>& src);

  /// Driver-quirk opt-out: when true, every layer property is
  /// re-emitted on every commit, bypassing the per-plane
  /// snapshot diff. Default false. Toggle on for drivers that refuse
  /// to inherit unwritten state across commits — empirically rare
  /// (we have no confirmed instance) but kept as a documented escape
  /// hatch for embedded stacks that might surface one. Survives
  /// across pause/resume; cleared on scene destruction.
  void set_force_full_property_writes(bool force) noexcept;
  [[nodiscard]] bool force_full_property_writes() const noexcept;

  // ── Session hooks ──────────────────────────────────────────────────
  //
  // Mirror drm::cursor::Renderer's session contract. The CRTC/
  // connector/mode binding and every layer handle survive across a
  // pause/resume pair; only fd-bound kernel state (plane registry,
  // property ids, MODE_ID blob, per-source GEM/FB handles) is rebuilt.

  /// The seat has been disabled. Forwards to every layer source's
  /// on_session_paused. No ioctls fire here.
  void on_session_paused() noexcept;

  /// The seat is back with `new_dev` (a freshly-opened Device against
  /// the new fd libseat handed the process). Re-enumerates the plane
  /// registry, re-caches property ids, rebuilds the allocator, and
  /// walks every live layer's source to re-allocate buffers against
  /// `new_dev`. Next commit implicitly carries ALLOW_MODESET to bring
  /// the CRTC back up. Layer handles (including generations) remain
  /// valid across the call; callers do not need to rebuild their
  /// handle maps.
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(drm::Device& new_dev);

  /// Rebind the scene to a new CRTC / connector / mode without
  /// destroying it. Used for hotplug-driven mode switches and
  /// CRTC migration on connector reassignment. Same fd, same set of
  /// live layer handles — the scene re-resolves CRTC index, refreshes
  /// the MODE_ID blob, re-caches the connector and CRTC properties,
  /// drops the allocator's warm state and per-plane property snapshot
  /// (kernel state from the old binding doesn't transfer), and forces
  /// the next commit to carry `ALLOW_MODESET`.
  ///
  /// Layer handles survive verbatim (same id, same generation). Layer
  /// sources keep their existing buffers — those are tied to the fd,
  /// not the CRTC. Returns a `CompatibilityReport` listing layers
  /// that don't look like they'll fit the new configuration; those
  /// entries don't block the rebind, they just signal what the
  /// caller should reposition or remove before the next commit.
  ///
  /// A failure here (e.g. property recache against the new CRTC
  /// fails, or registry re-enumeration fails) leaves the scene in a
  /// partially-rebound state; the caller should treat the scene as
  /// unusable and tear it down, rather than retrying.
  [[nodiscard]] drm::expected<CompatibilityReport, std::error_code> rebind(
      std::uint32_t new_crtc_id, std::uint32_t new_connector_id, drmModeModeInfo new_mode);

 private:
  class Impl;
  explicit LayerScene(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::scene
