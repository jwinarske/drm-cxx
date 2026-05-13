// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "scene_set.hpp"

#include "commit_report.hpp"
#include "layer_scene.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/modeset/atomic.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::scene {

class SceneSet::Impl {
 public:
  Impl(drm::Device& dev, std::vector<std::unique_ptr<LayerScene>> scenes) noexcept
      : dev_(&dev), scenes_(std::move(scenes)) {}

  [[nodiscard]] std::size_t scene_count() const noexcept { return scenes_.size(); }

  [[nodiscard]] LayerScene* scene(std::size_t index) noexcept {
    return (index < scenes_.size()) ? scenes_[index].get() : nullptr;
  }
  [[nodiscard]] const LayerScene* scene(std::size_t index) const noexcept {
    return (index < scenes_.size()) ? scenes_[index].get() : nullptr;
  }

  // Workhorse for both commit() and test(). The split mirrors
  // LayerScene::Impl::do_commit's split — build all, submit once,
  // finalize all — but spans every owned scene.
  [[nodiscard]] drm::expected<std::vector<CommitReport>, std::error_code> do_commit_all(
      std::uint32_t caller_flags, bool test_only, void* user_data) {
    std::vector<CommitReport> reports;
    reports.reserve(scenes_.size());
    if (scenes_.empty()) {
      return reports;
    }

    drm::AtomicRequest req(*dev_);
    if (!req.valid()) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
    }

    // Build every scene's writes onto the shared request.
    // Suspended scenes return a null FrameBuildPtr — we record a
    // sentinel so the post-commit finalize loop can emit a zero
    // CommitReport at the matching index without touching the scene.
    std::vector<FrameBuildPtr> states;
    states.reserve(scenes_.size());
    std::uint32_t combined_flags = caller_flags;
    for (auto& scene : scenes_) {
      auto build = scene->build_frame_into(req, caller_flags, test_only);
      if (!build) {
        // A scene's build failed: roll back every successful build by
        // running it through finalize_frame with the same error so
        // its acquisitions get released. Then propagate.
        const auto err = build.error();
        for (std::size_t i = 0; i < states.size(); ++i) {
          if (states[i] != nullptr) {
            (void)scenes_[i]->finalize_frame(std::move(states[i]),
                                             drm::unexpected<std::error_code>(err));
          }
        }
        return drm::unexpected<std::error_code>(err);
      }
      if (*build == nullptr) {
        // Scene is suspended: skip its participation in the commit.
        // The sentinel keeps the per-scene CommitReport vector
        // aligned with the constructor's scene order.
        states.push_back(FrameBuildPtr{});
        continue;
      }
      combined_flags |= LayerScene::effective_flags_of(**build);
      states.push_back(std::move(*build));
    }

    // One combined kernel commit covering every engaged scene's writes.
    drm::expected<void, std::error_code> kr;
    if (test_only) {
      kr = req.test(combined_flags);
    } else {
      kr = req.commit(combined_flags, user_data);
    }

    // Per-scene finalize. Every scene that produced an
    // engaged state sees the same kernel result — successes get the
    // post-commit state update (mark_clean / recorded placements),
    // EACCES on commit failure flips each participating scene's
    // suspended_ flag.
    for (std::size_t i = 0; i < scenes_.size(); ++i) {
      if (states[i] == nullptr) {
        reports.emplace_back();  // suspended scene: zero report
        continue;
      }
      auto r = scenes_[i]->finalize_frame(std::move(states[i]), kr);
      if (r) {
        reports.push_back(std::move(*r));
      } else {
        // finalize_frame surfacing an error is the kernel-commit
        // failure being reported back; record a zero report and
        // continue finalizing the remaining scenes so no scene leaks
        // acquisitions.
        reports.emplace_back();
      }
    }

    if (!kr) {
      return drm::unexpected<std::error_code>(kr.error());
    }
    return reports;
  }

 private:
  drm::Device* dev_;
  std::vector<std::unique_ptr<LayerScene>> scenes_;
};

SceneSet::SceneSet(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SceneSet::~SceneSet() = default;

drm::expected<std::unique_ptr<SceneSet>, std::error_code> SceneSet::create(
    drm::Device& dev, std::vector<std::unique_ptr<LayerScene>> scenes) {
  return std::unique_ptr<SceneSet>(new SceneSet(std::make_unique<Impl>(dev, std::move(scenes))));
}

std::size_t SceneSet::scene_count() const noexcept {
  return impl_->scene_count();
}

LayerScene* SceneSet::scene(std::size_t index) noexcept {
  return impl_->scene(index);
}

const LayerScene* SceneSet::scene(std::size_t index) const noexcept {
  return impl_->scene(index);
}

drm::expected<std::vector<CommitReport>, std::error_code> SceneSet::commit(std::uint32_t flags,
                                                                           void* user_data) {
  return impl_->do_commit_all(flags, /*test_only=*/false, user_data);
}

drm::expected<std::vector<CommitReport>, std::error_code> SceneSet::test() {
  return impl_->do_commit_all(0, /*test_only=*/true, /*user_data=*/nullptr);
}

}  // namespace drm::scene
