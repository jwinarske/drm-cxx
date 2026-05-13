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

class LayerScene;

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
  [[nodiscard]] drm::expected<std::vector<CommitReport>, std::error_code> commit(
      std::uint32_t flags = 0, void* user_data = nullptr);

  /// Build + issue the combined AtomicRequest as DRM_MODE_ATOMIC_TEST_ONLY.
  /// Same return shape as commit() but no kernel state is applied and
  /// no per-scene observable state mutates. Useful as a one-shot
  /// validator before committing for real.
  [[nodiscard]] drm::expected<std::vector<CommitReport>, std::error_code> test();

  [[nodiscard]] std::size_t scene_count() const noexcept;

  /// Non-owning access to a child scene by index. Returns nullptr for
  /// out-of-range indices; otherwise the pointer is valid for the
  /// SceneSet's lifetime.
  [[nodiscard]] LayerScene* scene(std::size_t index) noexcept;
  [[nodiscard]] const LayerScene* scene(std::size_t index) const noexcept;

 private:
  class Impl;
  explicit SceneSet(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::scene
