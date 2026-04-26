// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "../core/property_store.hpp"
#include "layer.hpp"
#include "plane_registry.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace drm {
class Device;
class AtomicRequest;
}  // namespace drm

namespace drm::planes {

struct CandidatePair {
  const PlaneCapabilities* plane;
  Layer* layer;
  int score;
};

// Test-commit failure cache: memoize (plane_id, property_hash) -> pass/fail
class TestCache {
 public:
  [[nodiscard]] std::optional<bool> lookup(uint32_t plane_id, std::size_t prop_hash) const;
  void record(uint32_t plane_id, std::size_t prop_hash, bool passed);
  [[nodiscard]] std::size_t hit_count(uint32_t plane_id, std::size_t prop_hash) const;
  void clear() noexcept;

 private:
  struct Entry {
    bool passed{};
    std::size_t hits{0};
  };
  std::map<std::pair<uint32_t, std::size_t>, Entry> cache_;
};

class Allocator {
 public:
  Allocator(const Device& dev, PlaneRegistry& registry);

  // Main entry point. Returns how many layers were hardware-assigned.
  //
  // `external_reserved` lists plane ids the *caller* will arm itself
  // (outside the allocator's plane-assignment loop) and that should
  // therefore be excluded from the disable-unused-planes pass —
  // without it, a plane the scene's composition fallback is about to
  // bind would be written FB_ID=0 / CRTC_ID=0 here only for the
  // caller to overwrite those properties immediately afterward, which
  // is wasteful and depends on kernel last-write-wins semantics. The
  // span is consumed during this call and not retained.
  drm::expected<std::size_t, std::error_code> apply(
      Output& output, AtomicRequest& req, uint32_t commit_flags,
      drm::span<const uint32_t> external_reserved = {});

  // Configurable test commit budget (default: 16)
  void set_max_test_commits(std::size_t max) noexcept;

  // Force every layer property to be re-emitted on every commit,
  // bypassing the per-plane "last committed" snapshot used by the
  // minimization path. Default false. Toggle on for drivers that
  // refuse to inherit unwritten state across commits — empirically
  // rare but observed on some embedded stacks. Always emits
  // FB_ID/CRTC_*/SRC_* for every assigned layer.
  void set_force_full_property_writes(bool force) noexcept { force_full_writes_ = force; }
  [[nodiscard]] bool force_full_property_writes() const noexcept { return force_full_writes_; }

  // Diagnostic counters from the most recent apply() call. Reset to
  // zero at the top of every apply(), so a caller observing these
  // immediately after a successful apply() sees that frame's totals.
  // Intended for CommitReport plumbing and benchmark assertions.
  struct Diagnostics {
    // Total property writes added to the caller's AtomicRequest by
    // the apply path: per-layer property-write skips that the
    // minimization filter let through, plus the disable-unused-planes
    // FB_ID=0 / CRTC_ID=0 emissions, plus internal compositor-layer
    // arming when the legacy composition_layer path fires.
    std::size_t properties_written{0};
    // Subset of properties_written: the count of FB_ID writes (any
    // value, zero or non-zero). Isolated because FB attachment is the
    // most expensive kernel-side action a commit can carry.
    std::size_t fbs_attached{0};
  };
  [[nodiscard]] Diagnostics diagnostics() const noexcept { return diagnostics_; }

  // Optional hook invoked before every internal TEST commit. Lets the
  // caller decorate each freshly built test request with state that must
  // be present for the TEST to succeed but isn't carried inside the
  // per-plane layer properties. Typical use: on the very first atomic
  // commit the CRTC is still inactive, so a plane-only request would
  // fail with EINVAL; the caller installs a preparer that attaches
  // MODE_ID / ACTIVE / connector.CRTC_ID when `flags` include
  // DRM_MODE_ATOMIC_ALLOW_MODESET. Returning an error aborts the TEST.
  using TestPreparer =
      std::function<drm::expected<void, std::error_code>(AtomicRequest&, uint32_t flags)>;
  void set_test_preparer(TestPreparer preparer);

 private:
  // §13.3 Warm-start: try previous frame's allocation
  drm::expected<std::size_t, std::error_code> apply_previous_allocation(Output& output,
                                                                        AtomicRequest& req,
                                                                        uint32_t flags,
                                                                        uint32_t crtc_index);

  // Full search with all improvements
  drm::expected<std::size_t, std::error_code> full_search(Output& output, AtomicRequest& req,
                                                          uint32_t flags, uint32_t crtc_index);

  // §13.5 Bipartite pre-solve
  std::vector<std::pair<Layer*, const PlaneCapabilities*>> bipartite_preseed(
      Output& output, uint32_t crtc_index) const;

  std::vector<std::pair<Layer*, const PlaneCapabilities*>> bipartite_preseed_group(
      std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
      uint32_t crtc_index) const;

  // §13.2 Best-first search order
  std::vector<CandidatePair> rank_candidates(Output& output, uint32_t crtc_index) const;

  [[nodiscard]] std::vector<CandidatePair> rank_candidates_group(
      const std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
      uint32_t crtc_index) const;

  [[nodiscard]] int score_pair(const PlaneCapabilities& plane, const Layer& layer) const;

  // §13.6 Content-type layer priority
  static int layer_priority(const Layer& layer);

  // §13.1 Static compatibility check (necessary conditions only)
  static bool plane_statically_compatible(const PlaneCapabilities& plane, const Layer& layer,
                                          uint32_t crtc_index);

  // §13.1 Static upper bound
  static int static_upper_bound(drm::span<Layer* const> remaining_layers,
                                const std::vector<const PlaneCapabilities*>& available_planes,
                                uint32_t crtc_index);

  // §13.7 Spatial intersection splitting
  static bool layers_intersect(const Layer& a, const Layer& b);
  static std::vector<std::vector<Layer*>> split_independent_groups(std::vector<Layer*>& layers);

  // Backtracking search
  bool backtrack(std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
                 std::unordered_map<uint32_t, Layer*>& assignment, std::size_t depth,
                 std::size_t best_so_far, AtomicRequest& req, uint32_t flags, uint32_t crtc_index);

  // Test-commit a tentative assignment. Builds a fresh AtomicRequest
  // internally that (a) disables every CRTC-compatible non-cursor plane
  // absent from `assignment`, then (b) applies each assigned layer's
  // properties. Running TEST without the disabled would inherit stale
  // state from the previously committed plane (old FB, CRTC_ID, zpos),
  // producing spurious EINVAL when migrating a layer between planes on
  // the same CRTC — two active planes, same zpos, same CRTC.
  bool try_test_commit(const std::unordered_map<uint32_t, Layer*>& assignment, uint32_t flags,
                       uint32_t crtc_index);

  // Apply layer properties to a plane in the atomic request — full
  // write path, no minimization. Used by try_test_commit (its
  // throwaway request has no kernel state to inherit from) and by the
  // legacy composition-layer arming.
  drm::expected<void, std::error_code> apply_layer_to_plane(const Layer& layer, uint32_t plane_id,
                                                            AtomicRequest& req) const;

  // Real-commit variant: writes only properties whose value differs
  // from the previous frame's snapshot for this plane (or every
  // property when the plane is freshly assigned, the layer pointer
  // changed, or `force_full_writes_` is on). Updates the snapshot on
  // success and bumps the apply-time diagnostics counters.
  drm::expected<void, std::error_code> apply_layer_to_plane_real(const Layer& layer,
                                                                 uint32_t plane_id,
                                                                 AtomicRequest& req);

  // Emit FB_ID=0 / CRTC_ID=0 on every CRTC-compatible non-cursor plane
  // that isn't in `keep`. Used both inside try_test_commit (so TESTs
  // reflect the final committed state) and after a winning assignment is
  // applied to the caller's request (so the COMMIT also clears dropped
  // planes instead of letting them inherit stale state).
  //
  // `track_state` controls whether the call also updates the
  // minimization snapshot and bumps diagnostic counters. True from
  // real-commit paths (apply()'s post-search disable pass and
  // apply_previous_allocation's warm-start disable pass); false from
  // try_test_commit, whose AtomicRequest is throwaway and whose
  // assignment is speculative. Non-const because true-mode mutates
  // last_committed_ and diagnostics_.
  void disable_unused_planes(AtomicRequest& req, uint32_t crtc_index,
                             const std::unordered_map<uint32_t, Layer*>& keep, bool track_state);

  const Device& dev_;
  PlaneRegistry& registry_;
  PropertyStore prop_store_;

  // §13.3 Previous allocation state
  bool previous_allocation_valid_{false};
  std::unordered_map<uint32_t, Layer*> previous_allocation_;

  // §13.4 Test-commit failure cache
  TestCache failure_cache_;

  std::size_t max_test_commits_{16};
  std::size_t test_commits_this_frame_{0};

  TestPreparer test_preparer_;

  // Transient view set at the top of apply() and cleared on exit.
  // Consumed by disable_unused_planes to keep caller-armed planes
  // (composition canvas, etc.) from being written FB_ID=0 / CRTC_ID=0
  // on every commit. Empty span outside an active apply() call.
  drm::span<const uint32_t> external_reserved_;

  // Per-plane snapshot of the layer + property map that was committed
  // last time this plane was armed. apply_layer_to_plane_real diffs
  // the new layer's properties against the snapshot and skips writes
  // whose value would be unchanged. Cleared for a plane when
  // disable_unused_planes detaches it (next assignment is a fresh
  // activation), and wholesale on every plane reassignment.
  struct LastCommitted {
    const Layer* layer{nullptr};
    Layer::PropertyMap properties;
  };
  std::unordered_map<uint32_t, LastCommitted> last_committed_;

  // True to disable per-property minimization. See
  // set_force_full_property_writes.
  bool force_full_writes_{false};

  // Reset at the top of every apply() and bumped from
  // apply_layer_to_plane_real / the real-commit disable_unused_planes
  // path. Read by callers via diagnostics() after a successful apply.
  Diagnostics diagnostics_{};
};

}  // namespace drm::planes
