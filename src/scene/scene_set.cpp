// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "scene_set.hpp"

#include "buffer_source.hpp"
#include "commit_report.hpp"
#include "detail/source_mirror.hpp"
#include "layer_desc.hpp"
#include "layer_handle.hpp"
#include "layer_scene.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/modeset/atomic.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace drm::scene {

namespace {

// LayerBufferSource forwarder for a layer on one scene. Owns a
// shared_ptr to the application-provided source and forwards every
// virtual call. A layer on several scenes goes through
// detail::SourceMirror instead.
class SharedLayerBufferSource final : public LayerBufferSource {
 public:
  explicit SharedLayerBufferSource(std::shared_ptr<LayerBufferSource> inner) noexcept
      : inner_(std::move(inner)) {}

  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override {
    return inner_->acquire();
  }
  void release(AcquiredBuffer acquired) noexcept override { inner_->release(std::move(acquired)); }
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
  void release_with_fence(AcquiredBuffer acquired,
                          std::optional<drm::sync::SyncFence> release_fence) noexcept override {
    inner_->release_with_fence(std::move(acquired), std::move(release_fence));
  }
  [[nodiscard]] bool wants_release_fence() const noexcept override {
    return inner_->wants_release_fence();
  }
  [[nodiscard]] bool has_fresh_content() const noexcept override {
    return inner_->has_fresh_content();
  }
  void on_retired() noexcept override { inner_->on_retired(); }
  [[nodiscard]] drm::expected<DmaBufDesc, std::error_code> export_dma_buf() override {
    return inner_->export_dma_buf();
  }

 private:
  std::shared_ptr<LayerBufferSource> inner_;
};

}  // namespace

namespace detail {

std::vector<std::vector<std::size_t>> partition_for_policy(const std::vector<SceneSlotState>& slots,
                                                           NarrowPolicy policy) {
  std::vector<std::vector<std::size_t>> groups;
  if (policy == NarrowPolicy::PerCrtc) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (!slots[i].is_hole) {
        groups.push_back({i});
      }
    }
    return groups;
  }
  if (policy == NarrowPolicy::AutoOnModeset) {
    std::vector<std::size_t> modeset_group;
    std::vector<std::size_t> steady_group;
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (slots[i].is_hole) {
        continue;
      }
      if (slots[i].wants_modeset) {
        modeset_group.push_back(i);
      } else {
        steady_group.push_back(i);
      }
    }
    // Mixed -> split (modeset first); uniform -> single combined group.
    if (!modeset_group.empty() && !steady_group.empty()) {
      groups.push_back(std::move(modeset_group));
      groups.push_back(std::move(steady_group));
    } else {
      std::vector<std::size_t> combined;
      combined.reserve(modeset_group.size() + steady_group.size());
      for (auto i : modeset_group) {
        combined.push_back(i);
      }
      for (auto i : steady_group) {
        combined.push_back(i);
      }
      if (!combined.empty()) {
        groups.push_back(std::move(combined));
      }
    }
    return groups;
  }
  // NarrowPolicy::Combined — one group with every non-hole slot.
  std::vector<std::size_t> combined;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    if (!slots[i].is_hole) {
      combined.push_back(i);
    }
  }
  if (!combined.empty()) {
    groups.push_back(std::move(combined));
  }
  return groups;
}

}  // namespace detail

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

  // ── Hotplug-driven set composition ───────────────────────────────
  //
  // add_scene / remove_scene keep scene indices stable across
  // mutations so previously-issued SetLayerHandles stay meaningful.
  // Removed slots are preserved as nullptr entries in scenes_; the
  // next add_scene reuses the lowest hole rather than appending.

  [[nodiscard]] drm::expected<std::size_t, std::error_code> add_scene(
      std::unique_ptr<LayerScene> scene) {
    if (!scene) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    for (std::size_t i = 0; i < scenes_.size(); ++i) {
      if (!scenes_[i]) {
        scenes_[i] = std::move(scene);
        seeded_.resize(scenes_.size(), false);
        seeded_[i] = false;
        return i;
      }
    }
    scenes_.push_back(std::move(scene));
    seeded_.resize(scenes_.size(), false);
    return scenes_.size() - 1;
  }

  void remove_scene(std::size_t index) noexcept {
    if (index >= scenes_.size() || !scenes_[index]) {
      return;
    }
    // Walk every set-level slot and drop pins that targeted this
    // scene. Other targets in the same slot stay live so a mirrored
    // SetLayerSpec across two scenes survives unplug of one of them.
    for (auto& slot : set_slots_) {
      if (!slot.alive) {
        continue;
      }
      auto& pins = slot.pins;
      pins.erase(std::remove_if(pins.begin(), pins.end(),
                                [index](const PerScenePin& p) { return p.scene_index == index; }),
                 pins.end());
    }
    // Its planes are free for the other scenes.
    plane_owner_.erase(std::remove_if(plane_owner_.begin(), plane_owner_.end(),
                                      [index](const PlaneClaim& c) { return c.owner == index; }),
                       plane_owner_.end());
    if (index < seeded_.size()) {
      seeded_[index] = false;
    }
    // Destroy the LayerScene last — its dtor releases buffers /
    // unbinds planes / etc., and we want every set-level pin already
    // removed by then so no LayerHandle-targeted remove_layer() runs
    // against a half-destroyed scene.
    scenes_[index].reset();
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
      if (t.scene_index >= scenes_.size() || !scenes_[t.scene_index]) {
        return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
      }
      if (!seen.insert(t.scene_index).second) {
        return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
      }
    }

    // Build the per-target LayerDescs + add to each scene. On the
    // first failure, roll back every successful add so the caller
    // sees an atomic outcome.
    // Several targets share one acquire per frame through a mirror, so a
    // per-frame ring shows the same frame on every CRTC.
    std::shared_ptr<detail::SourceMirror> mirror;
    if (spec.targets.size() > 1) {
      mirror = detail::SourceMirror::create(spec.source);
    }
    std::vector<PerScenePin> pins;
    pins.reserve(spec.targets.size());
    for (const auto& t : spec.targets) {
      LayerDesc desc;
      if (mirror) {
        desc.source = mirror->make_consumer();
      } else {
        desc.source = std::make_unique<SharedLayerBufferSource>(spec.source);
      }
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
      if (p.scene_index < scenes_.size() && scenes_[p.scene_index]) {
        scenes_[p.scene_index]->remove_layer(p.layer_handle);
      }
    }
    slot.pins.clear();
    slot.alive = false;
    free_set_slot_ids_.push_back(handle.id - 1);
  }

  // Snapshot every slot's hole / modeset state for the partition
  // planner. Pure read of scenes_ state; no destructive build.
  [[nodiscard]] std::vector<detail::SceneSlotState> snapshot_slots() const {
    std::vector<detail::SceneSlotState> slots;
    slots.reserve(scenes_.size());
    for (const auto& scene : scenes_) {
      if (scene) {
        slots.push_back({/*is_hole=*/false, scene->would_request_modeset()});
      } else {
        slots.push_back({/*is_hole=*/true, /*wants_modeset=*/false});
      }
    }
    return slots;
  }

  // Workhorse for both commit() and test(). Splits scenes into groups
  // per policy, builds every group's AtomicRequest up front, then
  // submits them in order: phase 1 = build all, phase 2 = commit all.
  // The two-phase shape lets us know which group's ioctl is the *last
  // engaged* one before any kernel call goes out, which matters for
  // PAGE_FLIP_EVENT routing — the kernel queues one event per atomic
  // commit that carries the flag, and a multi-group split with the
  // flag uniformly applied would fire the caller's handler N times per
  // logical SceneSet commit. We strip PAGE_FLIP_EVENT (and the
  // matching user_data pointer) from every non-final group so exactly
  // one kernel event ships per commit() call.
  //
  // When any group's build fails, every previously built group's
  // engaged scenes are finalized with that error and no kernel ioctl
  // ships — the two-phase shape recovers the rollback that the prior
  // build-and-commit-as-you-go implementation deliberately sacrificed.
  // If a kernel commit fails partway through phase 2, earlier
  // already-submitted commits still stand; that residual gap is
  // documented under AutoOnModeset.
  [[nodiscard]] drm::expected<std::vector<CommitReport>, std::error_code> do_commit_all(
      std::uint32_t caller_flags, bool test_only, void* user_data, NarrowPolicy policy) {
    std::vector<CommitReport> reports(scenes_.size());
    if (scenes_.empty()) {
      return reports;
    }

    const auto groups = detail::partition_for_policy(snapshot_slots(), policy);
    if (groups.empty()) {
      // Either every slot is a hole, or scenes_ is empty. Either way
      // the kernel commit would carry no writes — skip.
      return reports;
    }

    // Plane claims for this frame, starting from the committed ones. Each
    // scene builds with every plane another scene holds left out, then
    // claims what it armed: a plane several CRTCs share has one owner.
    seed_plane_owners();
    auto& claims = scratch_claims_;
    claims = plane_owner_;
    auto exclude_others = [&claims, this](std::size_t idx) {
      scratch_claimed_.clear();
      for (const auto& c : claims) {
        if (c.owner != idx) {
          scratch_claimed_.push_back(c.plane);
        }
      }
      std::sort(scratch_claimed_.begin(), scratch_claimed_.end());
      scenes_[idx]->set_planes_claimed_elsewhere(scratch_claimed_);
    };
    auto claim = [&claims](std::size_t idx, const FrameBuildState& state) {
      claims.erase(std::remove_if(claims.begin(), claims.end(),
                                  [idx](const PlaneClaim& c) { return c.owner == idx; }),
                   claims.end());
      for (const auto plane : LayerScene::armed_planes_of(state)) {
        auto it = std::find_if(claims.begin(), claims.end(),
                               [plane](const PlaneClaim& c) { return c.plane == plane; });
        if (it != claims.end()) {
          it->owner = idx;
        } else {
          claims.push_back({plane, idx});
        }
      }
    };

    struct EngagedBuild {
      std::size_t scene_index;
      FrameBuildPtr state;
    };
    struct GroupPending {
      drm::AtomicRequest req;
      std::vector<EngagedBuild> engaged;
      std::uint32_t combined_flags{0};
    };

    // Helper to finalize a list of engaged builds with a kernel result
    // (success or error). Used for rollback when a later group's build
    // fails — the kernel commit never went out for these scenes, so
    // we feed them an `unexpected` to release their acquisitions.
    auto finalize_with = [this, &reports](std::vector<EngagedBuild>& engaged,
                                          const drm::expected<void, std::error_code>& kr) {
      for (auto& eb : engaged) {
        auto r = scenes_[eb.scene_index]->finalize_frame(std::move(eb.state), kr);
        if (r) {
          reports[eb.scene_index] = std::move(*r);
        }
      }
    };

    // Phase 1: build every group's AtomicRequest. Groups that resolve
    // to zero engaged scenes are dropped — they'd skip the kernel
    // ioctl anyway, and dropping them keeps the "last engaged group"
    // index honest.
    std::vector<GroupPending> pending;
    pending.reserve(groups.size());
    for (const auto& group : groups) {
      GroupPending gp{drm::AtomicRequest{*dev_}, {}, caller_flags};
      if (!gp.req.valid()) {
        const auto err = std::make_error_code(std::errc::not_enough_memory);
        const drm::expected<void, std::error_code> rollback_kr{
            drm::unexpected<std::error_code>{err}};
        for (auto& prior : pending) {
          finalize_with(prior.engaged, rollback_kr);
        }
        return drm::unexpected<std::error_code>(err);
      }
      gp.engaged.reserve(group.size());
      for (auto idx : group) {
        auto& scene = scenes_[idx];
        if (!scene) {
          continue;  // hole — partition_for_policy already filtered, but be defensive
        }
        exclude_others(idx);
        auto build = scene->build_frame_into(gp.req, caller_flags, test_only);
        if (!build) {
          // Roll back this group's already-built engaged states plus
          // every prior group with the build error. No kernel commit
          // has run yet, so no kernel-side rollback is needed.
          const auto err = build.error();
          const drm::expected<void, std::error_code> rollback_kr{
              drm::unexpected<std::error_code>{err}};
          finalize_with(gp.engaged, rollback_kr);
          for (auto& prior : pending) {
            finalize_with(prior.engaged, rollback_kr);
          }
          return drm::unexpected<std::error_code>(err);
        }
        if (*build == nullptr) {
          continue;  // scene suspended
        }
        gp.combined_flags |= LayerScene::effective_flags_of(**build);
        claim(idx, **build);
        gp.engaged.push_back({idx, std::move(*build)});
      }
      if (!gp.engaged.empty()) {
        pending.push_back(std::move(gp));
      }
    }

    if (pending.empty()) {
      return reports;
    }

    // Phase 2: submit ioctls in order. Strip PAGE_FLIP_EVENT (and
    // user_data, which is only meaningful when the flag is set) from
    // every non-final group so exactly one flip event fires per
    // commit() call. test() goes through req.test() which already
    // masks PAGE_FLIP_EVENT internally — the strip below is a no-op
    // on that path but keeps both branches symmetric.
    std::error_code first_error;
    std::vector<bool> failed(scenes_.size(), false);
    const std::size_t last_idx = pending.size() - 1;
    for (std::size_t i = 0; i < pending.size(); ++i) {
      auto& gp = pending[i];
      const bool is_last = (i == last_idx);
      const std::uint32_t submit_flags =
          is_last ? gp.combined_flags
                  : (gp.combined_flags & ~static_cast<std::uint32_t>(DRM_MODE_PAGE_FLIP_EVENT));
      void* submit_user_data = is_last ? user_data : nullptr;

      drm::expected<void, std::error_code> kr;
      if (test_only) {
        kr = gp.req.test(submit_flags);
      } else {
        kr = gp.req.commit(submit_flags, submit_user_data);
      }
      if (!kr) {
        for (const auto& eb : gp.engaged) {
          failed[eb.scene_index] = true;
        }
      }
      finalize_with(gp.engaged, kr);
      if (!kr && !first_error) {
        first_error = kr.error();
      }
    }

    // Keep the claims of the scenes whose commit went through; a failed
    // scene keeps what it held, unless a committed scene took it.
    if (!test_only) {
      claims.erase(std::remove_if(claims.begin(), claims.end(),
                                  [&failed](const PlaneClaim& c) { return failed[c.owner]; }),
                   claims.end());
      for (const auto& c : plane_owner_) {
        const bool taken = std::any_of(claims.begin(), claims.end(),
                                       [&c](const PlaneClaim& k) { return k.plane == c.plane; });
        if (failed[c.owner] && !taken) {
          claims.push_back(c);
        }
      }
      std::swap(plane_owner_, claims);
    }

    if (first_error) {
      return drm::unexpected<std::error_code>(first_error);
    }
    return reports;
  }

 private:
  struct PlaneClaim {
    std::uint32_t plane{0};
    std::size_t owner{0};
  };

  // A scene that joins the set claims the planes the kernel shows lit on
  // its CRTC, so a scene warmed up alone keeps them.
  void seed_plane_owners() {
    seeded_.resize(scenes_.size(), false);
    drmModePlaneRes* res = nullptr;
    for (std::size_t i = 0; i < scenes_.size(); ++i) {
      if (!scenes_[i] || seeded_[i]) {
        continue;
      }
      if (res == nullptr) {
        res = drmModeGetPlaneResources(dev_->fd());
        if (res == nullptr) {
          return;  // retried next frame
        }
      }
      seeded_[i] = true;
      const auto crtc_id = scenes_[i]->crtc_id();
      for (std::uint32_t p = 0; p < res->count_planes; ++p) {
        drmModePlane* plane = drmModeGetPlane(dev_->fd(), res->planes[p]);
        if (plane == nullptr) {
          continue;
        }
        const bool owned =
            std::any_of(plane_owner_.begin(), plane_owner_.end(),
                        [plane](const PlaneClaim& c) { return c.plane == plane->plane_id; });
        if (plane->crtc_id == crtc_id && plane->fb_id != 0 && !owned) {
          plane_owner_.push_back({plane->plane_id, i});
        }
        drmModeFreePlane(plane);
      }
    }
    if (res != nullptr) {
      drmModeFreePlaneResources(res);
    }
  }

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
  // Committed owner (scene index) of each plane a scene holds, and the
  // frame's working copy (reused, so a steady frame does not allocate).
  std::vector<PlaneClaim> plane_owner_;
  std::vector<PlaneClaim> scratch_claims_;
  // Scenes whose lit planes were read into plane_owner_.
  std::vector<bool> seeded_;
  std::vector<std::uint32_t> scratch_claimed_;
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

drm::expected<std::size_t, std::error_code> SceneSet::add_scene(std::unique_ptr<LayerScene> scene) {
  return impl_->add_scene(std::move(scene));
}

void SceneSet::remove_scene(std::size_t index) {
  impl_->remove_scene(index);
}

drm::expected<std::vector<CommitReport>, std::error_code> SceneSet::commit(std::uint32_t flags,
                                                                           void* user_data,
                                                                           NarrowPolicy policy) {
  return impl_->do_commit_all(flags, /*test_only=*/false, user_data, policy);
}

drm::expected<std::vector<CommitReport>, std::error_code> SceneSet::test(NarrowPolicy policy) {
  return impl_->do_commit_all(0, /*test_only=*/true, /*user_data=*/nullptr, policy);
}

}  // namespace drm::scene
