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
  drm::expected<std::size_t, std::error_code> apply(Output& output, AtomicRequest& req,
                                                    uint32_t commit_flags);

  // Configurable test commit budget (default: 16)
  void set_max_test_commits(std::size_t max) noexcept;

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

  // Apply layer properties to a plane in the atomic request
  drm::expected<void, std::error_code> apply_layer_to_plane(const Layer& layer, uint32_t plane_id,
                                                            AtomicRequest& req) const;

  // Emit FB_ID=0 / CRTC_ID=0 on every CRTC-compatible non-cursor plane
  // that isn't in `keep`. Used both inside try_test_commit (so TESTs
  // reflect the final committed state) and after a winning assignment is
  // applied to the caller's request (so the COMMIT also clears dropped
  // planes instead of letting them inherit stale state).
  void disable_unused_planes(AtomicRequest& req, uint32_t crtc_index,
                             const std::unordered_map<uint32_t, Layer*>& keep) const;

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
};

}  // namespace drm::planes
