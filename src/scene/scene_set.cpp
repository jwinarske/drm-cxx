// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "scene_set.hpp"

#include "buffer_source.hpp"
#include "commit_report.hpp"
#include "layer_desc.hpp"
#include "layer_handle.hpp"
#include "layer_scene.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/modeset/atomic.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace drm::scene {

namespace {

// Per-scene LayerBufferSource forwarder. Each forwarder owns a
// shared_ptr to the application-provided underlying source and
// pass-through-forwards every virtual call. The underlying source's
// lifetime extends until the last forwarder is destroyed (which
// happens when the SceneSet drops the matching LayerHandles).
//
// Limitations match SceneSetLayerSpec's documented surface:
//   * acquire() / release() pass through verbatim — the underlying
//     source sees one call per participating scene per frame. Static-
//     buffer sources tolerate this; per-frame ring sources do not.
//   * on_session_resumed() is invoked once per participating scene
//     in the resume cycle. Resumable static-buffer sources tolerate
//     N back-to-back re-allocations.
class SharedLayerBufferSource final : public LayerBufferSource {
 public:
  explicit SharedLayerBufferSource(std::shared_ptr<LayerBufferSource> inner) noexcept
      : inner_(std::move(inner)) {}

  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override {
    return inner_->acquire();
  }
  void release(AcquiredBuffer acquired) noexcept override { inner_->release(acquired); }
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return inner_->binding_model();
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return inner_->format(); }
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> map(
      drm::MapAccess access) override {
    return inner_->map(access);
  }
  drm::expected<void, std::error_code> bind_to_plane(std::uint32_t plane_id) override {
    return inner_->bind_to_plane(plane_id);
  }
  void unbind_from_plane(std::uint32_t plane_id) noexcept override {
    inner_->unbind_from_plane(plane_id);
  }
  void on_session_paused() noexcept override { inner_->on_session_paused(); }
  drm::expected<void, std::error_code> on_session_resumed(const drm::Device& new_dev) override {
    return inner_->on_session_resumed(new_dev);
  }

 private:
  std::shared_ptr<LayerBufferSource> inner_;
};

}  // namespace

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

  // ── SceneSet-level layer routing ─────────────────────────────────
  //
  // SetLayerHandle.id encodes a 1-based slot index into set_slots_;
  // generation guards against use-after-remove. Each slot tracks the
  // per-scene LayerHandles produced when the SetLayerSpec was added,
  // so remove_layer can walk back and remove each.

  [[nodiscard]] drm::expected<SetLayerHandle, std::error_code> add_set_layer(
      const SceneSetLayerSpec& spec) {
    if (!spec.source) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    if (spec.targets.empty()) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    // Validate target indices are in range and unique. Duplicates
    // would route two LayerDescs onto the same scene under one
    // SetLayerHandle, which makes remove_layer's per-target loop
    // ambiguous and serves no real use case.
    std::unordered_set<std::size_t> seen;
    seen.reserve(spec.targets.size());
    for (const auto& t : spec.targets) {
      if (t.scene_index >= scenes_.size()) {
        return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
      }
      if (!seen.insert(t.scene_index).second) {
        return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
      }
    }

    // Build the per-target LayerDescs + add to each scene. On the
    // first failure, roll back every successful add so the caller
    // sees an atomic outcome.
    std::vector<PerScenePin> pins;
    pins.reserve(spec.targets.size());
    for (const auto& t : spec.targets) {
      LayerDesc desc;
      desc.source = std::make_unique<SharedLayerBufferSource>(spec.source);
      desc.display = t.display;
      desc.force_composited = t.force_composited;

      auto h = scenes_[t.scene_index]->add_layer(std::move(desc));
      if (!h) {
        // Roll back successful entries.
        for (const auto& p : pins) {
          scenes_[p.scene_index]->remove_layer(p.layer_handle);
        }
        return drm::unexpected<std::error_code>(h.error());
      }
      pins.push_back(PerScenePin{t.scene_index, *h});
    }

    // Allocate (or recycle) a slot.
    SetLayerSlot* slot = nullptr;
    std::uint32_t slot_idx = 0;
    if (!free_set_slot_ids_.empty()) {
      slot_idx = free_set_slot_ids_.back();
      free_set_slot_ids_.pop_back();
      slot = &set_slots_[slot_idx];
    } else {
      set_slots_.emplace_back();
      slot_idx = static_cast<std::uint32_t>(set_slots_.size() - 1);
      slot = &set_slots_.back();
    }
    slot->pins = std::move(pins);
    slot->alive = true;
    slot->generation += 1;

    SetLayerHandle handle;
    handle.id = slot_idx + 1;
    handle.generation = slot->generation;
    return handle;
  }

  void remove_set_layer(SetLayerHandle handle) noexcept {
    if (handle.id == 0 || handle.id > set_slots_.size()) {
      return;
    }
    auto& slot = set_slots_[handle.id - 1];
    if (!slot.alive || slot.generation != handle.generation) {
      return;
    }
    for (const auto& p : slot.pins) {
      if (p.scene_index < scenes_.size()) {
        scenes_[p.scene_index]->remove_layer(p.layer_handle);
      }
    }
    slot.pins.clear();
    slot.alive = false;
    free_set_slot_ids_.push_back(handle.id - 1);
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
  struct PerScenePin {
    std::size_t scene_index{0};
    LayerHandle layer_handle{};
  };

  struct SetLayerSlot {
    std::vector<PerScenePin> pins;
    std::uint32_t generation{0};
    bool alive{false};
  };

  drm::Device* dev_;
  std::vector<std::unique_ptr<LayerScene>> scenes_;
  std::vector<SetLayerSlot> set_slots_;
  std::vector<std::uint32_t> free_set_slot_ids_;
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

drm::expected<SetLayerHandle, std::error_code> SceneSet::add_layer(const SceneSetLayerSpec& spec) {
  return impl_->add_set_layer(spec);
}

void SceneSet::remove_layer(SetLayerHandle handle) {
  impl_->remove_set_layer(handle);
}

drm::expected<std::vector<CommitReport>, std::error_code> SceneSet::commit(std::uint32_t flags,
                                                                           void* user_data) {
  return impl_->do_commit_all(flags, /*test_only=*/false, user_data);
}

drm::expected<std::vector<CommitReport>, std::error_code> SceneSet::test() {
  return impl_->do_commit_all(0, /*test_only=*/true, /*user_data=*/nullptr);
}

}  // namespace drm::scene
