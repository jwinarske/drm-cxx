// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// scene_set.hpp — multi-CRTC coordinator over N LayerScenes.
//
// Owns a fixed list of LayerScene instances (one per CRTC the
// application drives) and batches their per-frame property writes
// into a single drm::AtomicRequest, then issues one combined kernel
// commit. The kernel reports success only if every CRTC's slice
// passes its modeset / placement checks together — that's the
// tear-free synchronization primitive a coordinated multi-display
// app (instrument cluster, video wall, mirrored content) needs.
//
// Callers add layers to each child scene directly via
// `scene_set.scene(i)->add_layer(...)`.
//
// Hot path (3 scenes, real commit):
//   1. Caller invokes SceneSet::commit(flags, user_data).
//   2. SceneSet allocates one drm::AtomicRequest against the shared fd.
//   3. For each child scene, SceneSet calls
//      LayerScene::build_frame_into(req, ...), which appends the
//      scene's property writes onto req and returns a FrameBuildPtr
//      (or nullptr to signal the scene is suspended — skipped).
//   4. SceneSet OR-combines effective_flags_of(*state) across the
//      engaged scenes and issues ONE drmModeAtomicCommit.
//   5. For each engaged scene, SceneSet calls
//      LayerScene::finalize_frame(state, kr) so per-layer mark_clean
//      / recorded placements / FB release / suspended_-on-EACCES run.
//
// Same shape for test() but with DRM_MODE_ATOMIC_TEST_ONLY and no
// per-scene state mutation.

#pragma once

#include "commit_report.hpp"
#include "display_params.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

class LayerBufferSource;
class LayerScene;

/// Cross-scene layer specification used by SceneSet::add_layer. One
/// shared buffer source rides one or more child scenes; each entry in
/// `targets` names a scene index and the DisplayParams (rect, zpos,
/// alpha, rotation) for the layer on that scene. Multiple targets =
/// mirrored / per-output specialized; one target = single-output.
///
/// The shared_ptr keeps the underlying source alive across every
/// participating scene. SceneSet wraps the source in a per-target
/// internal forwarder so each scene's LayerScene::add_layer receives
/// its own LayerBufferSource pointer.
///
/// Limitations:
///   * The underlying source must accept multiple acquire/release
///     pairs per logical frame — N participating scenes call
///     acquire() / release() once each per commit. Static-buffer
///     sources (DumbBufferSource, ExternalDmaBufSource) handle this
///     naturally. Per-frame ring sources (V4L2 decoder, GstAppsink)
///     do NOT support mirroring in this revision.
///   * on_session_resumed() fires once per participating scene, so
///     the underlying source's resume hook must tolerate repeated
///     calls within a single VT-switch cycle. DumbBufferSource and
///     ExternalDmaBufSource do; GBM-backed sources have not been
///     audited for this pattern yet.
struct SceneSetLayerSpec {
  std::shared_ptr<LayerBufferSource> source;

  struct Target {
    std::size_t scene_index{0};
    DisplayParams display{};
    /// Mirror of `LayerDesc::force_composited`: when true the layer
    /// is forced through CompositeCanvas on this scene regardless of
    /// the allocator's natural placement decision. Useful for HUD-
    /// style overlays that the application wants explicitly composed.
    bool force_composited{false};
  };

  std::vector<Target> targets;
};

/// Cross-CRTC commit grouping policy for `SceneSet::commit` / `test`.
///
/// The library's default is `AutoOnModeset`: every frame rides one
/// combined `drmModeAtomicCommit` covering every engaged scene, except
/// when one or more scenes need `ALLOW_MODESET` for the upcoming
/// commit and others do not. In that case the modeset-needing scenes
/// commit first as their own combined group, and the steady-state
/// scenes commit second as a separate combined group — so a hotplug
/// or rebind on one CRTC doesn't pull unrelated CRTCs through a
/// modeset transition (visible on amdgpu / i915 as a brief blank).
///
/// `Combined` forces the pre-policy single-commit behavior; useful
/// when the application accepts the modeset disturbance for the sake
/// of strict cross-CRTC tear-free sync on every frame.
///
/// `PerCrtc` issues one commit per engaged scene unconditionally. No
/// cross-CRTC sync. Intended for diagnostics and for apps that drive
/// strictly-independent outputs from one SceneSet (a typical instrument
/// cluster does not).
enum class NarrowPolicy : std::uint8_t {
  Combined,       // never split; one ioctl per commit() call
  AutoOnModeset,  // split only when ALLOW_MODESET state is mixed
  PerCrtc,        // split unconditionally, one ioctl per engaged scene
};

namespace detail {

/// Snapshot of a single SceneSet slot used by the partition planner.
/// `is_hole` mirrors `scenes_[i] == nullptr` (after a `remove_scene`
/// or never-filled); `wants_modeset` mirrors the corresponding
/// scene's `LayerScene::would_request_modeset()` peek.
/// `wants_modeset` is ignored when `is_hole == true`.
struct SceneSlotState {
  bool is_hole{false};
  bool wants_modeset{false};
};

/// Pure partition planner. Returns the per-group submission order
/// `SceneSet::commit` should follow for the given policy + slot
/// states. Holes never appear in any group; engaged-ish scenes
/// (non-hole) always appear in exactly one group. Modeset-needing
/// scenes go first under `AutoOnModeset` when the policy splits.
///
/// Pure and exposed for unit testing — production callers go
/// through `SceneSet::commit` / `test`.
[[nodiscard]] std::vector<std::vector<std::size_t>> partition_for_policy(
    const std::vector<SceneSlotState>& slots, NarrowPolicy policy);

}  // namespace detail

/// Opaque handle for a SceneSet-level layer. Returned by
/// SceneSet::add_layer; passed to SceneSet::remove_layer. Generation-
/// tagged so stale handles (after remove_layer, slot recycling) are
/// safe no-ops. Default-constructed handles are always invalid.
struct SetLayerHandle {
  std::uint32_t id{0};
  std::uint32_t generation{0};
  [[nodiscard]] bool valid() const noexcept { return id != 0; }
  [[nodiscard]] friend bool operator==(const SetLayerHandle& a, const SetLayerHandle& b) noexcept {
    return a.id == b.id && a.generation == b.generation;
  }
};

class SceneSet {
 public:
  /// Build a SceneSet that owns the supplied LayerScenes. All scenes
  /// must have been constructed against the same drm::Device — the
  /// SceneSet's AtomicRequest is bound to its fd, so a mix of
  /// fd-tied scenes would fail with -EBADF at commit time.
  ///
  /// Empty scene lists are allowed and produce a no-op SceneSet whose
  /// commit() / test() return an empty vector.
  [[nodiscard]] static drm::expected<std::unique_ptr<SceneSet>, std::error_code> create(
      drm::Device& dev, std::vector<std::unique_ptr<LayerScene>> scenes);

  ~SceneSet();

  SceneSet(const SceneSet&) = delete;
  SceneSet& operator=(const SceneSet&) = delete;
  SceneSet(SceneSet&&) = delete;
  SceneSet& operator=(SceneSet&&) = delete;

  /// Build a combined drm::AtomicRequest covering every owned scene's
  /// frame writes and submit one drmModeAtomicCommit. Returns one
  /// CommitReport per child scene in construction order. Suspended
  /// scenes are represented by a default-constructed CommitReport.
  ///
  /// `flags` is OR-ed with the implicit flags every scene's build
  /// pass collected (ALLOW_MODESET on a scene's first commit /
  /// colorspace change / HDR metadata change). The combined commit
  /// inherits the union — if any scene needs ALLOW_MODESET, the
  /// whole commit gets it.
  ///
  /// `user_data` is forwarded to drmModeAtomicCommit verbatim.
  /// Callers wiring PAGE_FLIP_EVENT typically pass their PageFlip*
  /// here; the kernel routes it back as user_data on the next vblank.
  /// When `policy` splits the commit into more than one group, every
  /// resulting kernel commit carries the same `user_data` — callers
  /// using PAGE_FLIP_EVENT receive one event per CRTC per group, so
  /// the total event count still matches the engaged scene count.
  [[nodiscard]] drm::expected<std::vector<CommitReport>, std::error_code> commit(
      std::uint32_t flags = 0, void* user_data = nullptr,
      NarrowPolicy policy = NarrowPolicy::AutoOnModeset);

  /// Build + issue the AtomicRequest(s) as DRM_MODE_ATOMIC_TEST_ONLY.
  /// Same return shape as commit() but no kernel state is applied and
  /// no per-scene observable state mutates. `policy` controls
  /// grouping the same way it does in commit(); the default mirrors
  /// the runtime behavior so a TEST that passes is a faithful
  /// preview of the matching commit().
  [[nodiscard]] drm::expected<std::vector<CommitReport>, std::error_code> test(
      NarrowPolicy policy = NarrowPolicy::AutoOnModeset);

  /// Add a layer that rides one or more child scenes. For each target
  /// the SceneSet builds a LayerDesc wrapping `spec.source` in a
  /// per-target forwarder and calls `scene(target.scene_index)->add_layer`.
  ///
  /// Rejections (no partial state retained):
  ///   * `std::errc::invalid_argument` — `spec.source` is null,
  ///     `spec.targets` is empty, or any `target.scene_index` is out
  ///     of range / duplicated within `targets`.
  ///   * any error returned by `LayerScene::add_layer` is propagated
  ///     after rolling back already-added layers on earlier targets.
  [[nodiscard]] drm::expected<SetLayerHandle, std::error_code> add_layer(
      const SceneSetLayerSpec& spec);

  /// Remove every per-scene layer named by `handle`. Stale handles
  /// (already removed, slot recycled) are silent no-ops.
  void remove_layer(SetLayerHandle handle);

  [[nodiscard]] std::size_t scene_count() const noexcept;

  /// Non-owning access to a child scene by index. Returns nullptr for
  /// out-of-range indices and for slots vacated by `remove_scene` (the
  /// vector keeps stable indices across remove/add so live SetLayerHandles
  /// don't shift); otherwise the pointer is valid until the next
  /// `remove_scene` or SceneSet destruction.
  [[nodiscard]] LayerScene* scene(std::size_t index) noexcept;
  [[nodiscard]] const LayerScene* scene(std::size_t index) const noexcept;

  /// Application-driven hotplug: attach a new child scene to the set.
  /// The caller-supplied `scene` must already be constructed against
  /// the same `drm::Device` the SceneSet was created with (mismatched
  /// devices fail with `-EBADF` on the next combined commit; the set
  /// does not introspect to enforce this up front).
  ///
  /// New scenes fill the lowest-index hole left by a previous
  /// `remove_scene`, if any; otherwise the scene is appended and the
  /// vector grows by one. The returned index is the slot the scene
  /// occupies and is stable until the slot is explicitly removed.
  ///
  /// Recommended usage: the application's hotplug handler builds a
  /// fresh `LayerScene` (typically after probing the new connector),
  /// runs one single-CRTC `LayerScene::commit()` to clear the implicit
  /// `ALLOW_MODESET` from first commit, then hands the warmed scene to
  /// `add_scene` so the next `SceneSet::commit()` doesn't promote the
  /// whole combined commit to modeset mode. See docs/multi_output.md
  /// for the full warmup pattern.
  ///
  /// Rejections:
  ///   * `std::errc::invalid_argument` — `scene` is null.
  [[nodiscard]] drm::expected<std::size_t, std::error_code> add_scene(
      std::unique_ptr<LayerScene> scene);

  /// Application-driven hotplug: detach the child scene at `index` and
  /// destroy it. Walks every active SetLayerSpec and drops any pins
  /// that target this scene; the underlying shared `LayerBufferSource`
  /// stays alive for other targets it rides. SetLayerHandles that
  /// retain pins on other scenes remain valid; ones whose every target
  /// was on the removed scene become non-functional but still safe
  /// (subsequent `remove_layer` calls are no-ops).
  ///
  /// Out-of-range indices and already-removed slots are silent no-ops.
  /// The slot itself is preserved as a hole — `scene(index)` returns
  /// nullptr, `scene_count()` still reports `index + 1` until a
  /// later `add_scene` reuses the hole. This keeps every other
  /// scene's index stable across the call.
  void remove_scene(std::size_t index);

 private:
  class Impl;
  explicit SceneSet(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::scene
