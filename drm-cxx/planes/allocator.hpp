// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/property_store.hpp"
#include "layer.hpp"
#include "output.hpp"
#include "plane_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <span>
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
  std::optional<bool> lookup(uint32_t plane_id, std::size_t prop_hash) const;
  void record(uint32_t plane_id, std::size_t prop_hash, bool passed);
  std::size_t hit_count(uint32_t plane_id, std::size_t prop_hash) const;
  void clear() noexcept;

 private:
  struct Entry {
    bool passed;
    std::size_t hits{0};
  };
  std::map<std::pair<uint32_t, std::size_t>, Entry> cache_;
};

class Allocator {
 public:
  Allocator(const Device& dev, PlaneRegistry& registry);

  // Main entry point. Returns how many layers were hardware-assigned.
  std::expected<std::size_t, std::error_code> apply(Output& output, AtomicRequest& req,
                                                    uint32_t commit_flags);

  // Configurable test commit budget (default: 16)
  void set_max_test_commits(std::size_t max) noexcept;

 private:
  // §13.3 Warm-start: try previous frame's allocation
  std::expected<std::size_t, std::error_code> apply_previous_allocation(Output& output,
                                                                        AtomicRequest& req,
                                                                        uint32_t flags);

  // Full search with all improvements
  std::expected<std::size_t, std::error_code> full_search(Output& output, AtomicRequest& req,
                                                          uint32_t flags);

  // §13.5 Bipartite pre-solve
  std::vector<std::pair<Layer*, const PlaneCapabilities*>> bipartite_preseed(Output& output,
                                                                             uint32_t crtc_index);

  std::vector<std::pair<Layer*, const PlaneCapabilities*>> bipartite_preseed_group(
      std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
      uint32_t crtc_index);

  // §13.2 Best-first search order
  std::vector<CandidatePair> rank_candidates(Output& output, uint32_t crtc_index) const;

  std::vector<CandidatePair> rank_candidates_group(
      const std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
      uint32_t crtc_index) const;

  int score_pair(const PlaneCapabilities& plane, const Layer& layer) const;

  // §13.6 Content-type layer priority
  int layer_priority(const Layer& layer) const;

  // §13.1 Static compatibility check (necessary conditions only)
  bool plane_statically_compatible(const PlaneCapabilities& plane, const Layer& layer,
                                   uint32_t crtc_index) const;

  // §13.1 Static upper bound
  int static_upper_bound(std::span<Layer* const> remaining_layers,
                         const std::vector<const PlaneCapabilities*>& available_planes,
                         uint32_t crtc_index) const;

  // §13.7 Spatial intersection splitting
  bool layers_intersect(const Layer& a, const Layer& b) const;
  std::vector<std::vector<Layer*>> split_independent_groups(std::vector<Layer*>& layers) const;

  // Backtracking search
  bool backtrack(std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
                 std::unordered_map<uint32_t, Layer*>& assignment, std::size_t depth,
                 std::size_t best_so_far, AtomicRequest& req, uint32_t flags, uint32_t crtc_index);

  // Build atomic request from assignment and test-commit
  bool try_test_commit(const std::unordered_map<uint32_t, Layer*>& assignment, Output& output,
                       AtomicRequest& req, uint32_t flags);

  // Apply layer properties to a plane in the atomic request
  std::expected<void, std::error_code> apply_layer_to_plane(const Layer& layer, uint32_t plane_id,
                                                            AtomicRequest& req);

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
};

}  // namespace drm::planes
