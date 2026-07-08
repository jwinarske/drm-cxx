// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "../core/property_store.hpp"
#include "layer.hpp"
#include "matching.hpp"
#include "plane_registry.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace drm {
class Device;
class AtomicRequest;
}  // namespace drm

namespace drm::planes {

/// Power-aware placement bonus for a buffer's modifier bandwidth class:
/// Compression → 2, Tiling → 1, Linear → 0. Folded into `score_pair` so the
/// matcher keeps compressed/tiled layers on planes (compositing them costs a GPU
/// decompress) and composites the cheap LINEAR ones when planes are contested.
/// A small tiebreak below the structural scores. Exposed for unit testing.
[[nodiscard]] int bandwidth_class_bonus(std::uint64_t modifier) noexcept;

struct CandidatePair {
  const PlaneCapabilities* plane;
  Layer* layer;
  int score;
};

// Flat plane→layer assignment table. Backed by a sorted-by-insertion-
// order `std::vector<pair<uint32_t, Layer*>>` with the small subset of
// `unordered_map` API the allocator actually uses. For the ≤8-plane
// CRTCs we see in practice, linear scan beats unordered_map's
// bucket + node allocations, and per-frame copy-assignment becomes a
// `vector::operator=` that reuses destination capacity instead of
// rehashing.
//
// API matches the parts of `std::unordered_map<uint32_t, Layer*>` the
// allocator hits: empty / size / clear / reserve, begin / end (so
// range-for + structured bindings work), count / find / erase, and
// insert_or_assign. Operator[] is deliberately absent — every caller
// uses insert_or_assign already, and dropping the operator avoids the
// "default-construct then assign" trap that nullable pointer values
// would silently absorb.
class PlaneAssignment {
 public:
  using value_type = std::pair<uint32_t, Layer*>;
  using iterator = std::vector<value_type>::iterator;
  using const_iterator = std::vector<value_type>::const_iterator;

  PlaneAssignment() = default;
  PlaneAssignment(const PlaneAssignment&) = default;
  PlaneAssignment(PlaneAssignment&&) noexcept = default;
  PlaneAssignment& operator=(const PlaneAssignment&) = default;
  PlaneAssignment& operator=(PlaneAssignment&&) noexcept = default;
  ~PlaneAssignment() = default;

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  void clear() noexcept { entries_.clear(); }
  void reserve(std::size_t n) { entries_.reserve(n); }

  [[nodiscard]] iterator begin() noexcept { return entries_.begin(); }
  [[nodiscard]] iterator end() noexcept { return entries_.end(); }
  [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }

  [[nodiscard]] std::size_t count(uint32_t plane_id) const noexcept {
    return find(plane_id) != entries_.end() ? 1U : 0U;
  }

  [[nodiscard]] iterator find(uint32_t plane_id) noexcept {
    return std::find_if(entries_.begin(), entries_.end(),
                        [plane_id](const value_type& e) { return e.first == plane_id; });
  }

  [[nodiscard]] const_iterator find(uint32_t plane_id) const noexcept {
    return std::find_if(entries_.begin(), entries_.end(),
                        [plane_id](const value_type& e) { return e.first == plane_id; });
  }

  void insert_or_assign(uint32_t plane_id, Layer* layer) {
    auto it = find(plane_id);
    if (it != entries_.end()) {
      it->second = layer;
    } else {
      entries_.emplace_back(plane_id, layer);
    }
  }

  iterator erase(const_iterator it) noexcept { return entries_.erase(it); }

  std::size_t erase(uint32_t plane_id) noexcept {
    auto it = find(plane_id);
    if (it == entries_.end()) {
      return 0;
    }
    entries_.erase(it);
    return 1;
  }

 private:
  std::vector<value_type> entries_;
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
  // `test_only` plumbs the caller's intent to TEST vs COMMIT down to
  // the per-plane snapshot cache (`last_committed_`). On TEST_ONLY
  // we must NOT update the cache — the kernel hasn't actually
  // applied the state, so a subsequent commit (or TEST) that diffs
  // against the snapshot would suppress properties the kernel still
  // requires (most visibly: CRTC_ID + dest/src rect under MODESET).
  drm::expected<std::size_t, std::error_code> apply(
      Output& output, AtomicRequest& req, uint32_t commit_flags,
      drm::span<const uint32_t> external_reserved = {}, bool test_only = false);

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

  // Erase any cached state that references `layer`. Must be called
  // before the planes::Layer pointed to by `layer` is destroyed,
  // because the allocator's per-plane snapshot stores a raw pointer
  // for layer-identity checks; without invalidation, heap reuse can
  // give a freshly-added layer the same address, fool the diff path
  // into treating the two as the same logical layer, and silently
  // skip property writes the kernel needs.
  void forget_layer(const Layer* layer) noexcept;

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
    // DRM_MODE_ATOMIC_TEST_ONLY commits issued during this apply() while
    // probing for a viable plane assignment. 1 in steady state (warm
    // re-validation of the cached assignment); higher when the cache
    // misses and full_search has to preseed/greedy/backtrack.
    std::size_t test_commits_issued{0};
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
  drm::expected<std::size_t, std::error_code> apply_previous_allocation(
      Output& output, AtomicRequest& req, uint32_t flags, uint32_t crtc_index, bool test_only);

  // Full search with all improvements
  drm::expected<std::size_t, std::error_code> full_search(Output& output, AtomicRequest& req,
                                                          uint32_t flags, uint32_t crtc_index,
                                                          bool test_only);

  // §13.5 Bipartite pre-solve
  std::vector<std::pair<Layer*, const PlaneCapabilities*>> bipartite_preseed(
      Output& output, uint32_t crtc_index) const;

  std::vector<std::pair<Layer*, const PlaneCapabilities*>> bipartite_preseed_group(
      const std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
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

  // True if a prior single-plane TEST_ONLY proved this layer's (fourcc, modifier)
  // can't scan out on this plane (a lying IN_FORMATS advertisement). Consulted
  // alongside the property-hash failure_cache when filtering edge candidates.
  [[nodiscard]] bool probe_rejected(uint32_t crtc_index, uint32_t plane_id,
                                    const Layer& layer) const;

  // §13.1 Static upper bound
  static int static_upper_bound(drm::span<Layer* const> remaining_layers,
                                const std::vector<const PlaneCapabilities*>& available_planes,
                                uint32_t crtc_index);

  // §13.7 Spatial intersection splitting
  static bool layers_intersect(const Layer& a, const Layer& b);
  static std::vector<std::vector<Layer*>> split_independent_groups(std::vector<Layer*>& layers);

  // Backtracking search
  bool backtrack(std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
                 PlaneAssignment& assignment, std::size_t depth, std::size_t best_so_far,
                 AtomicRequest& req, uint32_t flags, uint32_t crtc_index);

  // Place a single layer set on a single plane set. Runs preseed →
  // greedy → backtrack-drop-one (lowest priority first) and returns
  // the largest subset that passes a TEST commit; empty when no
  // non-empty subset survives or the per-frame TEST budget is hit.
  // Used both by the per-group pass and by the scene-wide partial
  // fallback in full_search.
  PlaneAssignment place_group(const std::vector<Layer*>& layers,
                              const std::vector<const PlaneCapabilities*>& planes, uint32_t flags,
                              uint32_t crtc_index);

  // Pick the layer with the fewest statically compatible planes among
  // `planes` (lowest layer_priority breaks ties). Returns the
  // bottleneck layer the scene-wide fallback should drop next, or
  // nullptr when `layers` is empty.
  static const Layer* pick_most_constrained(const std::vector<Layer*>& layers,
                                            const std::vector<const PlaneCapabilities*>& planes,
                                            uint32_t crtc_index);

  // Test-commit a tentative assignment. Builds a fresh AtomicRequest
  // internally that (a) disables every CRTC-compatible non-cursor plane
  // absent from `assignment`, then (b) applies each assigned layer's
  // properties. Running TEST without the disabled would inherit stale
  // state from the previously committed plane (old FB, CRTC_ID, zpos),
  // producing spurious EINVAL when migrating a layer between planes on
  // the same CRTC — two active planes, same zpos, same CRTC.
  // Returns `std::error_code{}` on success, the kernel's actual ioctl
  // error otherwise. Callers that only care pass/fail can rely on the
  // `error_code`'s implicit bool conversion (`if (auto ec = ...; !ec)`
  // for "passed"). Preserving the real errno matters specifically for
  // `apply_previous_allocation`, which propagates EACCES upward so the
  // caller can soft-pause on master loss instead of seeing every test
  // failure flattened into EAGAIN.
  std::error_code try_test_commit(const PlaneAssignment& assignment, uint32_t flags,
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
                                                                 AtomicRequest& req,
                                                                 bool test_only);

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
  void disable_unused_planes(AtomicRequest& req, uint32_t crtc_index, const PlaneAssignment& keep,
                             bool track_state, bool test_only);

  // True when the plane's advertised zpos range is degenerate
  // (zpos_min == zpos_max), i.e. the slot is fixed and zpos is not
  // actually settable. Some drivers (e.g. vc4's PRIMARY, range [0,0])
  // expose such a property WITHOUT the DRM_MODE_PROP_IMMUTABLE flag, so
  // is_immutable() doesn't catch it; the kernel's atomic_check still
  // rejects any write with EINVAL. The apply paths skip the zpos write
  // for these planes so a fixed-slot plane (notably a UI layer assigned
  // to the primary) doesn't poison the whole commit.
  [[nodiscard]] bool zpos_is_fixed(uint32_t plane_id) const;

  const Device& dev_;
  PlaneRegistry& registry_;
  PropertyStore prop_store_;

  // Cache of the Output's CRTC id → its index in drmModeRes::crtcs[],
  // resolved via drmModeGetResources. Avoids that ioctl on every apply()
  // (the index is stable for the fd's lifetime). Keyed by crtc id so a
  // caller that retargets the Output to a different CRTC re-resolves.
  std::optional<uint32_t> cached_crtc_index_;
  uint32_t cached_crtc_index_id_{0};

  // §13.3 Previous allocation state
  bool previous_allocation_valid_{false};
  PlaneAssignment previous_allocation_;

  // §13.4 Test-commit failure cache
  TestCache failure_cache_;
  // Modifier-level probe cache: when a single-plane TEST_ONLY fails for a
  // (crtc, plane, fourcc, modifier) that passed IN_FORMATS, it is recorded
  // Rejected so a lying IN_FORMATS entry (e.g. over-advertised AFBC) costs one
  // probe, not a dropped edge re-probed every frame. Persists for the allocator's
  // lifetime like failure_cache_; keyed on stable ids, so entries left behind by a
  // hotplug are inert (never looked up again).
  drm::fmt::ModifierProbeCache probe_cache_;

  std::size_t max_test_commits_{16};
  std::size_t test_commits_this_frame_{0};

  TestPreparer test_preparer_;

  // Owning copy of the caller's external_reserved set, populated at
  // the top of apply() and cleared on exit. Previously held as a
  // `drm::span` aliasing caller storage — fine in the current call
  // shape, but the contract wasn't enforceable: any future refactor
  // that moved apply() onto a coroutine or threadpool could see the
  // caller's vector reallocate between span capture and span read.
  // Reserved planes are O(1) per scene (typically 0–2), so the copy
  // cost is negligible. Vector picked over fixed-size array so a
  // caller passing >N reserved planes doesn't silently truncate.
  std::vector<uint32_t> external_reserved_;

  // Per-plane snapshot of the layer + property map that was committed
  // last time this plane was armed. apply_layer_to_plane_real diffs
  // the new layer's properties against the snapshot and skips writes
  // whose value would be unchanged. Cleared for a plane when
  // disable_unused_planes detaches it (next assignment is a fresh
  // activation), and wholesale on every plane reassignment.
  struct LastCommitted {
    const Layer* layer{nullptr};
    Layer::PropertySnapshot properties;
  };
  std::unordered_map<uint32_t, LastCommitted> last_committed_;

  // True to disable per-property minimization. See
  // set_force_full_property_writes.
  bool force_full_writes_{false};

  // Per-frame scratch reused across apply() calls. Capacity carries
  // forward, but contents are reset / cleared at the top of each
  // user of these so a fresh call sees the same invariants as a
  // local. Mutable so the const bipartite_preseed_group path can
  // still re-shape scratch_matching_ without giving up its
  // const-correctness contract.
  mutable BipartiteMatching scratch_matching_;
  std::vector<Layer*> scratch_placeable_;

  // Set of Layer*s present in the current apply() call's
  // `output.layers()`. Populated at the top of apply(); read by the
  // has_new_layer pre-pass and by apply_previous_allocation's stale-
  // layer prune. Replaces the previous O(N×M) nested-loop membership
  // checks with O(1) lookups; the set itself is O(N) to build and
  // reuses its bucket array across frames.
  std::unordered_set<const Layer*> scratch_current_set_;

  // Reset at the top of every apply() and bumped from
  // apply_layer_to_plane_real / the real-commit disable_unused_planes
  // path. Read by callers via diagnostics() after a successful apply.
  Diagnostics diagnostics_{};
};

}  // namespace drm::planes
