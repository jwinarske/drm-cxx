// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// layer_scene.hpp — top-level scene façade.
//
// Constructs from (Device, CRTC/connector/mode); manages layers by
// handle (monotonic + generation); runs a single test() / commit()
// path through drm::planes::Allocator against a fresh AtomicRequest;
// surfaces diagnostics through CommitReport. Property minimization,
// composition fallback for unassigned layers, rebind() across
// CRTC/connector/mode changes, and page-flip async completion are
// all wired through this façade as well.
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
#include <drm-cxx/detail/span.hpp>

#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

namespace drm {
class AtomicRequest;
class Device;
}  // namespace drm

namespace drm::sync {
class SyncFence;
}  // namespace drm::sync

namespace drm::display {
struct HdrSourceMetadata;
}  // namespace drm::display

namespace drm::scene {

class FrameBuildState;

// Hides FrameBuildState's destructor inside layer_scene.cpp so the
// public header can keep the type opaque while still letting callers
// hold std::unique_ptr to it.
struct FrameBuildStateDeleter {
  void operator()(FrameBuildState* state) const noexcept;
};

using FrameBuildPtr = std::unique_ptr<FrameBuildState, FrameBuildStateDeleter>;

class LayerScene {
 public:
  struct Config {
    std::uint32_t crtc_id{0};
    std::uint32_t connector_id{0};
    drmModeModeInfo mode{};

    /// EGL Streams capability for this scene. Defaults to
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

  /// Look up a layer by the opaque `identity_tag` passed in its
  /// `LayerDesc`. Returns nullptr if no currently-live layer has the
  /// given tag (or the tag is nullptr, since nullptr is the unset
  /// sentinel and matching against it would return an arbitrary
  /// non-tagged layer). When multiple live layers share a tag — which
  /// callers should avoid but the scene does not police — the first
  /// match in slot order is returned. Lookup is a linear scan over the
  /// live slot table; the IVI workload that motivates this method
  /// typically has 5–15 layers, where the linear scan beats a hashmap
  /// probe on cache traffic.
  [[nodiscard]] Layer* find_by_identity_tag(void* tag) noexcept;
  [[nodiscard]] const Layer* find_by_identity_tag(void* tag) const noexcept;

  [[nodiscard]] std::size_t layer_count() const noexcept;

  /// The stream capability the scene was constructed with. Callers
  /// inspect this to decide whether to add `EglStreamSource`-backed
  /// layers (when `mixing != StreamMixingMode::Unsupported`) and to
  /// branch the producer-side wiring on the extension set the driver
  /// exposes. Survives `rebind()` and pause/resume verbatim — the
  /// capability describes the driver, not the connector or CRTC.
  /// The `mixing` field may be upgraded from `Exclusive` to `Mixed`
  /// by a successful `probe_stream_mixing()` call.
  [[nodiscard]] const StreamCapability& stream_capability() const noexcept;

  /// DRM format modifiers any non-cursor plane on this scene's CRTC
  /// accepts for `drm_format`. Used to negotiate which modifier a
  /// GBM-surface or Vulkan/EGL producer should target — pass the
  /// returned list to `eglQueryDmaBufModifiersEXT` /
  /// `VkDrmFormatModifierPropertiesListEXT`, intersect, pick one,
  /// then construct a `GbmSurfaceSource` with that single modifier.
  ///
  /// The list is the *union* across candidate planes — any modifier
  /// in the returned set will be accepted by at least one plane the
  /// allocator might pick for the layer. Duplicates are removed; the
  /// order matches the order modifiers appear in IN_FORMATS (driver
  /// preference). `DRM_FORMAT_MOD_LINEAR` is included when at least
  /// one plane exposes it (explicitly or via the IN_FORMATS LINEAR
  /// entry); `DRM_FORMAT_MOD_INVALID` is included only when the
  /// driver doesn't expose IN_FORMATS at all (older legacy stacks).
  ///
  /// Returns an empty vector when no plane on this CRTC supports
  /// `drm_format` at all, or when the CRTC index can't be resolved
  /// (rare — only after `rebind()` failure).
  [[nodiscard]] std::vector<std::uint64_t> candidate_modifiers(std::uint32_t drm_format) const;

  /// Run the stream-layer plane-pin pre-pass that normally fires
  /// inside `commit()`. After this returns, every alive
  /// `DriverOwnsBinding` layer has its source bound to a plane (or
  /// has logged a failure), and `EglStreamSource::producer_surface()`
  /// returns a usable handle.
  ///
  /// Callers driving the NVIDIA-Streams flow use this to obtain the
  /// producer surface BEFORE the first commit so they can render a
  /// first frame; `commit()` then routes the atomic request through
  /// `eglStreamConsumerAcquireAttribKHR` with
  /// `EGL_DRM_ATOMIC_REQUEST_NV`, handing the first-frame acquire +
  /// commit submission off to the driver.
  ///
  /// Idempotent: subsequent calls are a no-op for slots already
  /// pinned. Safe to call before any frame has been committed.
  void prepare_stream_layers();

  /// Run an empirical TEST atomic commit that pairs an already-bound
  /// stream consumer plane with a temporary FB-ID-attached plane on
  /// the same CRTC. On kernel acceptance the scene's cached
  /// `StreamCapability::mixing` upgrades from `Exclusive` to `Mixed`
  /// and the result is sticky for the rest of the scene's lifetime
  /// (cleared on `rebind()` / `on_session_resumed()`).
  ///
  /// Preconditions:
  ///
  ///   * `stream_capability().usable()` must be true.
  ///   * At least one alive layer must have a `DriverOwnsBinding`
  ///     source that has already been committed once (i.e. has been
  ///     pinned to a plane and `bind_to_plane()` has succeeded).
  ///     Without an existing stream binding the probe has nothing to
  ///     test against.
  ///
  /// Returns the current mixing mode on success, regardless of
  /// whether it was upgraded. The probe is informational; callers
  /// who get back `Exclusive` know the driver enforces the
  /// single-stream-layer-per-CRTC restriction on this hardware.
  ///
  /// Error returns:
  ///
  ///   * `errc::function_not_supported` — capability is `Unsupported`
  ///     or no stream layer is currently bound to a plane.
  ///   * `errc::resource_unavailable_try_again` — no DRM plane is
  ///     available on the CRTC to serve as the probe's FB-ID target
  ///     (every non-cursor plane is already in use or reserved).
  ///   * `errc::not_enough_memory` — failed to allocate the probe's
  ///     scratch dumb buffer or atomic request.
  ///
  /// Idempotent on subsequent calls: once the probe has run (success
  /// or kernel rejection), the cached value is returned without
  /// re-issuing the TEST commit.
  drm::expected<StreamMixingMode, std::error_code> probe_stream_mixing();

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
  /// `out_fence` (opt-in): when non-null and the CRTC advertises OUT_FENCE_PTR,
  /// the real commit fills it with a sync_file that signals once this frame is
  /// scanned out. The caller owns it (it closes the fd) and typically waits on
  /// it before reusing the just-committed buffer. Left untouched on test commits
  /// or drivers without OUT_FENCE_PTR.
  [[nodiscard]] drm::expected<CommitReport, std::error_code> commit(
      std::uint32_t flags = 0, void* user_data = nullptr,
      drm::sync::SyncFence* out_fence = nullptr);

  // ── SceneSet integration (advanced) ─────────────────────────────────
  //
  // Two-phase commit primitive. drm::scene::SceneSet uses this to
  // batch N scenes' property writes into one cross-CRTC atomic
  // commit; standard applications call commit()/test() and don't
  // touch the build_frame_into / finalize_frame surface directly.
  //
  // Contract:
  //   1. Caller constructs a drm::AtomicRequest.
  //   2. For each scene call build_frame_into(req, caller_flags,
  //      test_only). Each call appends this scene's property writes
  //      to `req` and returns a FrameBuildState (or null if the
  //      scene is suspended — skip it for this cycle).
  //   3. Caller OR-combines effective_flags_of(*state) across all
  //      engaged scenes and issues ONE req.commit() / req.test()
  //      against `req`.
  //   4. For each engaged scene call finalize_frame(state, kr) with
  //      the kernel result. finalize_frame reconciles per-layer
  //      state (mark_clean, recorded placements, FB release, the
  //      suspended_ flag on EACCES) and returns the CommitReport.
  //
  // build_frame_into already releases the scene's own acquisitions
  // on its internal error paths (e.g. acquire_all failed). The
  // caller is responsible for finalizing only those scenes that
  // returned engaged states.
  //
  // A returned FrameBuildState that is destroyed without going
  // through finalize_frame leaks the held acquisitions — callers
  // MUST finalize unless build_frame_into itself returned an error.
  [[nodiscard]] drm::expected<FrameBuildPtr, std::error_code> build_frame_into(
      drm::AtomicRequest& req, std::uint32_t caller_flags, bool test_only);

  [[nodiscard]] drm::expected<CommitReport, std::error_code> finalize_frame(
      FrameBuildPtr state, drm::expected<void, std::error_code> kernel_result);

  /// Inspect the OR-combined `flags` the scene's build pass requires
  /// (caller's flags OR-ed with implicit ALLOW_MODESET on first
  /// commit / colorspace change / HDR metadata change). Callers
  /// merging multiple scenes into one commit collect these and OR
  /// them before passing the combined flags to req.commit().
  [[nodiscard]] static std::uint32_t effective_flags_of(const FrameBuildState& state) noexcept;

  /// Conservative pre-build peek: would the next `build_frame_into`
  /// pass OR `DRM_MODE_ATOMIC_ALLOW_MODESET` into its effective_flags?
  /// Returns true for the cases knowable without running the build —
  /// `first_commit_` pending (after `create()` / `rebind()` / a
  /// session-resume cycle), or user-set HDR / colorspace state that
  /// hasn't been written yet.
  ///
  /// Returns false for cases that depend on layer content (auto-
  /// derived colorspace / HDR signaling changes from the current
  /// layer set) — those would still flip `effective_flags` during
  /// the actual build, but only the build pass itself can observe
  /// them. Treat false as "probably not, but may still escalate."
  ///
  /// Used by `SceneSet`'s `NarrowPolicy::AutoOnModeset` to decide
  /// whether to partition the combined commit into a modeset-needing
  /// group and a steady-state group before any destructive work runs.
  [[nodiscard]] bool would_request_modeset() const noexcept;

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

  /// Request the CRTC's variable-refresh-rate toggle (VRR_ENABLED). Opt-in;
  /// off until called. The scene arms it on every commit and ORs ALLOW_MODESET
  /// into the flags when the value changes (amdgpu validates the timing
  /// reconfigure only under modeset). Drivers/CRTCs without VRR_ENABLED (probe
  /// `DriverProfile::vrr_capable`) silently swallow the call — no state change,
  /// no error. Pairs with the idle-Skip: VRR matches the flip cadence while
  /// content updates, Skip stops flips entirely when it's static.
  void set_vrr_enabled(bool enable);

  /// amdgpu output transfer function (`AMD_CRTC_REGAMMA_TF`) — the output stage
  /// of amdgpu's DRM/KMS color-pipeline uAPI, used for HDR / wide-gamut output.
  /// The scene resolves the requested function against the CRTC property's enum
  /// list by name and arms it each commit, ORing ALLOW_MODESET when it changes.
  /// CRTCs without the property (non-amdgpu, older kernels) silently swallow the
  /// call. This is the output stage; the per-plane stages
  /// (`AMD_PLANE_{DEGAMMA,SHAPER,LUT3D,BLEND}_*`, `HDR_MULT`) are driven through
  /// `DisplayParams::amd_color`.
  enum class OutputTransferFunction : std::uint8_t {
    Default,  ///< driver default (no regamma applied)
    Identity,
    Srgb,     ///< amdgpu enum "sRGB inv_EOTF"
    Bt709,    ///< "BT.709 OETF"
    Pq,       ///< "PQ inv_EOTF" — HDR10 output
    Gamma22,  ///< "Gamma 2.2 inv_EOTF"
    Gamma24,  ///< "Gamma 2.4 inv_EOTF"
    Gamma26,  ///< "Gamma 2.6 inv_EOTF"
  };
  void set_output_transfer_function(OutputTransferFunction tf);

  /// Driver-quirk opt-out: when true, every layer property is
  /// re-emitted on every commit, bypassing the per-plane
  /// snapshot diff. Default false. Toggle on for drivers that refuse
  /// to inherit unwritten state across commits — empirically rare
  /// (we have no confirmed instance) but kept as a documented escape
  /// hatch for embedded stacks that might surface one. Survives
  /// across pause/resume; cleared on scene destruction.
  void set_force_full_property_writes(bool force) noexcept;
  [[nodiscard]] bool force_full_property_writes() const noexcept;

  /// Planes armed by something OUTSIDE this scene that the allocator
  /// must never disable in its disable-unused-planes pass. The
  /// canonical case: a CRTC with no dedicated cursor plane, where
  /// drm::cursor::Renderer takes an overlay this scene would otherwise
  /// treat as unused — disabling it every commit fights the cursor's
  /// own commits (visible as cursor/primary flicker on motion). Pass
  /// the cursor's plane id here so the scene leaves it alone. Replaces
  /// the full set on each call; pass an empty span to clear. Survives
  /// pause/resume and rebind (stored on the scene, re-read every frame).
  void set_external_reserved_planes(drm::span<const std::uint32_t> planes);

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
  // FrameBuildState and its deleter need to name LayerScene::Impl
  // (private nested type) when constructing/destroying state out of
  // line; friending is scoped to those two helpers so the rest of the
  // SceneSet path goes through the public build_frame_into / finalize_frame
  // surface above.
  friend class FrameBuildState;
  friend struct FrameBuildStateDeleter;
  explicit LayerScene(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::scene
