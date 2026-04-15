// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "allocator.hpp"

#include "../core/device.hpp"
#include "../modeset/atomic.hpp"
#include "matching.hpp"
#include "planes/layer.hpp"
#include "planes/output.hpp"
#include "planes/plane_registry.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace drm::planes {

// ── TestCache ──────────────────────────────────────────────────

std::optional<bool> TestCache::lookup(uint32_t plane_id, std::size_t prop_hash) const {
  auto it = cache_.find({plane_id, prop_hash});
  if (it == cache_.end()) {
    return std::nullopt;
  }
  return it->second.passed;
}

void TestCache::record(uint32_t plane_id, std::size_t prop_hash, bool passed) {
  auto& entry = cache_[{plane_id, prop_hash}];
  entry.passed = passed;
  entry.hits++;
}

std::size_t TestCache::hit_count(uint32_t plane_id, std::size_t prop_hash) const {
  auto it = cache_.find({plane_id, prop_hash});
  if (it == cache_.end()) {
    return 0;
  }
  return it->second.hits;
}

void TestCache::clear() noexcept {
  cache_.clear();
}

// ── Allocator ──────────────────────────────────────────────────

Allocator::Allocator(const Device& dev, PlaneRegistry& registry) : dev_(dev), registry_(registry) {}

void Allocator::set_max_test_commits(std::size_t max) noexcept {
  max_test_commits_ = max;
}

// ── Main entry point (§13.3 warm-start logic) ──────────────────

drm::expected<std::size_t, std::error_code> Allocator::apply(Output& output, AtomicRequest& req,
                                                             uint32_t commit_flags) {
  test_commits_this_frame_ = 0;

  // Reset layer assignment state
  for (auto* layer : output.layers()) {
    layer->needs_composition_ = false;
    layer->assigned_plane_ = std::nullopt;
  }

  // Fast path: nothing changed since last frame
  if (!output.any_layer_dirty() && previous_allocation_valid_) {
    return apply_previous_allocation(output, req, commit_flags);
  }

  // Warm-start: try previous allocation first (one test commit)
  if (previous_allocation_valid_) {
    auto result = apply_previous_allocation(output, req, commit_flags);
    if (result.has_value()) {
      output.mark_clean();
      return result;
    }
  }

  // Full search
  return full_search(output, req, commit_flags);
}

// ── Warm-start from previous frame ────────────────────────────

drm::expected<std::size_t, std::error_code> Allocator::apply_previous_allocation(Output& output,
                                                                                 AtomicRequest& req,
                                                                                 uint32_t flags) {
  // Validate that all layer pointers from previous allocation still exist
  auto& current_layers = output.layers();
  for (auto it = previous_allocation_.begin(); it != previous_allocation_.end();) {
    bool found = false;
    for (const auto* l : current_layers) {
      if (l == it->second) {
        found = true;
        break;
      }
    }
    if (!found) {
      it = previous_allocation_.erase(it);
    } else {
      ++it;
    }
  }

  if (previous_allocation_.empty()) {
    return drm::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  if (try_test_commit(previous_allocation_, output, req, flags)) {
    // Apply previous assignment to layers
    std::size_t assigned = 0;
    for (auto& [plane_id, layer] : previous_allocation_) {
      layer->assigned_plane_ = plane_id;
      layer->needs_composition_ = false;
      ++assigned;
    }
    // Mark unassigned layers as needing composition
    for (auto* layer : output.layers()) {
      if (!layer->assigned_plane_.has_value() && !layer->is_composition_layer()) {
        layer->needs_composition_ = true;
      }
    }
    output.mark_clean();
    return assigned;
  }
  return drm::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
}

// ── Full search with all improvements ─────────────────────────

drm::expected<std::size_t, std::error_code> Allocator::full_search(Output& output,
                                                                   AtomicRequest& req,
                                                                   uint32_t flags) {
  // Determine CRTC index from the output's CRTC id
  // For now, we pass the crtc_id and let compatibility checks handle it.
  // The crtc_index is the bit position in possible_crtcs.
  // We need to find it from the DRM resources, but for the allocator
  // we just use the crtc_id and check against all planes.

  // Find crtc_index by checking which planes are compatible
  uint32_t crtc_index = 0;
  // Simple heuristic: find from first compatible plane
  auto all_planes = registry_.all();
  // We'll try all possible crtc indices (0..31) — in practice <=4
  for (uint32_t idx = 0; idx < 32; ++idx) {
    bool found = false;
    for (const auto& p : all_planes) {
      if (p.compatible_with_crtc(idx)) {
        found = true;
        break;
      }
    }
    if (found) {
      crtc_index = idx;
      break;
    }
  }

  output.sort_layers_by_zpos();

  // §13.7 Spatial intersection splitting
  auto groups = split_independent_groups(output.layers());

  auto available_planes = registry_.for_crtc(crtc_index);

  std::unordered_map<uint32_t, Layer*> best_assignment;
  std::size_t total_assigned = 0;

  for (auto& group : groups) {
    // §13.5 Bipartite pre-solve for this group
    auto preseed = bipartite_preseed_group(group, available_planes, crtc_index);

    // Build initial assignment from preseed
    std::unordered_map<uint32_t, Layer*> assignment;
    for (auto& [layer, plane] : preseed) {
      assignment[plane->id] = layer;
    }

    // Test the preseed assignment
    if (!assignment.empty()) {
      // Check via atomic test
      AtomicRequest test_req(dev_);
      bool const preseed_ok = try_test_commit(assignment, output, test_req, flags);

      if (!preseed_ok) {
        // §13.2 + §13.1 Backtrack from preseed with best-first ordering
        assignment.clear();
        auto candidates = rank_candidates_group(group, available_planes, crtc_index);

        // Simple greedy assignment from ranked candidates
        std::unordered_map<uint32_t, bool> used_planes;
        std::unordered_map<Layer*, bool> assigned_layers;

        for (const auto& cand : candidates) {
          if (used_planes.count(cand.plane->id) != 0) {
            continue;
          }
          if (assigned_layers.count(cand.layer) != 0) {
            continue;
          }

          // Check failure cache
          auto cached = failure_cache_.lookup(cand.plane->id, cand.layer->property_hash());
          if (cached.has_value() && !*cached) {
            continue;
          }

          assignment[cand.plane->id] = cand.layer;
          used_planes[cand.plane->id] = true;
          assigned_layers[cand.layer] = true;
        }

        // Test this greedy assignment
        AtomicRequest greedy_req(dev_);
        if (!try_test_commit(assignment, output, greedy_req, flags)) {
          // Backtrack: remove assignments one by one from the end
          auto assigned_vec =
              std::vector<std::pair<uint32_t, Layer*>>(assignment.begin(), assignment.end());

          // Sort by layer priority (lowest priority dropped first)
          std::sort(assigned_vec.begin(), assigned_vec.end(), [](const auto& a, const auto& b) {
            return layer_priority(*a.second) < layer_priority(*b.second);
          });

          for (auto& it : assigned_vec) {
            assignment.erase(it.first);

            AtomicRequest bt_req(dev_);
            if (try_test_commit(assignment, output, bt_req, flags)) {
              break;
            }

            if (test_commits_this_frame_ >= max_test_commits_) {
              break;
            }
          }
        }
      }
    }

    // Apply this group's assignment
    for (auto& [plane_id, layer] : assignment) {
      best_assignment[plane_id] = layer;
      ++total_assigned;
    }

    // Remove used planes from available
    for (auto& kv : assignment) {
      const uint32_t plane_id = kv.first;
      available_planes.erase(
          std::remove_if(available_planes.begin(), available_planes.end(),
                         [plane_id](const PlaneCapabilities* p) { return p->id == plane_id; }),
          available_planes.end());
    }
  }

  // Apply the best assignment
  for (auto& [plane_id, layer] : best_assignment) {
    layer->assigned_plane_ = plane_id;
    layer->needs_composition_ = false;
    if (auto r = apply_layer_to_plane(*layer, plane_id, req); !r) {
      return drm::unexpected(r.error());
    }
  }

  // Mark unassigned layers
  for (auto* layer : output.layers()) {
    if (!layer->assigned_plane_.has_value() && !layer->is_composition_layer()) {
      layer->needs_composition_ = true;
    }
  }

  // Ensure composition layer is on primary if any layer needs composition
  bool any_composited = false;
  for (const auto* layer : output.layers()) {
    if (layer->needs_composition()) {
      any_composited = true;
      break;
    }
  }

  if (any_composited && (output.composition_layer() != nullptr)) {
    // Find primary plane for this crtc
    for (const auto* plane : registry_.for_crtc(crtc_index)) {
      if (plane->type == DRMPlaneType::PRIMARY && best_assignment.count(plane->id) == 0) {
        output.composition_layer()->assigned_plane_ = plane->id;
        if (auto r = apply_layer_to_plane(*output.composition_layer(), plane->id, req); !r) {
          return drm::unexpected(r.error());
        }
        break;
      }
    }
  }

  // Save for warm-start next frame
  previous_allocation_ = best_assignment;
  previous_allocation_valid_ = true;

  output.mark_clean();
  return total_assigned;
}

// ── §13.5 Bipartite pre-solve ─────────────────────────────────

std::vector<std::pair<Layer*, const PlaneCapabilities*>> Allocator::bipartite_preseed(
    Output& output, uint32_t crtc_index) {
  return bipartite_preseed_group(output.layers(), registry_.for_crtc(crtc_index), crtc_index);
}

// Helper: preseed for a group of layers and available planes
std::vector<std::pair<Layer*, const PlaneCapabilities*>> Allocator::bipartite_preseed_group(
    std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
    uint32_t crtc_index) {
  std::vector<std::pair<Layer*, const PlaneCapabilities*>> result;

  if (layers.empty() || planes.empty()) {
    return result;
  }

  BipartiteMatching matching(layers.size(), planes.size());

  for (std::size_t i = 0; i < layers.size(); ++i) {
    for (std::size_t j = 0; j < planes.size(); ++j) {
      if (plane_statically_compatible(*planes[j], *layers[i], crtc_index)) {
        int const s = score_pair(*planes[j], *layers[i]);
        matching.add_edge(i, j, s);
      }
    }
  }

  matching.solve();

  for (std::size_t i = 0; i < layers.size(); ++i) {
    auto m = matching.match_for_left(i);
    if (m.has_value()) {
      result.emplace_back(layers[i], planes[*m]);
    }
  }

  return result;
}

// ── §13.2 Best-first search order ─────────────────────────────

std::vector<CandidatePair> Allocator::rank_candidates(Output& output, uint32_t crtc_index) const {
  return rank_candidates_group(output.layers(), registry_.for_crtc(crtc_index), crtc_index);
}

std::vector<CandidatePair> Allocator::rank_candidates_group(
    const std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
    uint32_t crtc_index) const {
  std::vector<CandidatePair> pairs;
  for (const auto* plane : planes) {
    for (auto* layer : layers) {
      if (plane_statically_compatible(*plane, *layer, crtc_index)) {
        pairs.push_back({plane, layer, score_pair(*plane, *layer)});
      }
    }
  }
  std::sort(pairs.begin(), pairs.end(),
            [](const CandidatePair& a, const CandidatePair& b) { return a.score > b.score; });
  return pairs;
}

int Allocator::score_pair(const PlaneCapabilities& plane, const Layer& layer) const {
  int s = 0;

  // Prefer matching format
  auto fmt = layer.format();
  if (fmt && plane.supports_format(*fmt)) {
    s += 4;
  }

  // Composition layer strongly prefers primary plane
  if (layer.is_composition_layer() && plane.type == DRMPlaneType::PRIMARY) {
    s += 8;
  }

  // Non-composition layers prefer overlay planes
  if (!layer.is_composition_layer() && plane.type == DRMPlaneType::OVERLAY) {
    s += 2;
  }

  // §13.6 Content-type priority
  s += layer_priority(layer) / 10;

  // Penalize previously failed combinations
  s -= static_cast<int>(failure_cache_.hit_count(plane.id, layer.property_hash()));

  return s;
}

// ── §13.6 Content-type layer priority ─────────────────────────

int Allocator::layer_priority(const Layer& layer) {
  if (layer.content_type() == ContentType::Video) {
    return 100;
  }
  if (layer.update_hz() > 30) {
    return 80;
  }
  if (layer.update_hz() > 0) {
    return 50;
  }
  return 10;
}

// ── §13.1 Static compatibility ────────────────────────────────

bool Allocator::plane_statically_compatible(const PlaneCapabilities& plane, const Layer& layer,
                                            uint32_t crtc_index) {
  if (!plane.compatible_with_crtc(crtc_index)) {
    return false;
  }

  auto fmt = layer.format();
  if (fmt && !plane.supports_format(*fmt)) {
    return false;
  }

  if (layer.rotation() != 0 && !plane.supports_rotation) {
    return false;
  }

  if (layer.requires_scaling() && !plane.supports_scaling) {
    return false;
  }

  auto z = layer.property("zpos");
  if (z.has_value()) {
    if (plane.zpos_min && *z < *plane.zpos_min) {
      return false;
    }
    if (plane.zpos_max && *z > *plane.zpos_max) {
      return false;
    }
  }

  if (plane.type == DRMPlaneType::CURSOR) {
    if (plane.cursor_max_w > 0 && layer.width() > plane.cursor_max_w) {
      return false;
    }
    if (plane.cursor_max_h > 0 && layer.height() > plane.cursor_max_h) {
      return false;
    }
  }

  return true;
}

int Allocator::static_upper_bound(drm::span<Layer* const> remaining_layers,
                                  const std::vector<const PlaneCapabilities*>& available_planes,
                                  uint32_t crtc_index) {
  int bound = 0;
  for (const Layer* layer : remaining_layers) {
    bool const any = std::any_of(available_planes.begin(), available_planes.end(),
                                 [&](const PlaneCapabilities* p) {
                                   return plane_statically_compatible(*p, *layer, crtc_index);
                                 });
    if (any) {
      ++bound;
    }
  }
  return bound;
}

// ── §13.7 Spatial intersection splitting ──────────────────────

bool Allocator::layers_intersect(const Layer& a, const Layer& b) {
  auto ra = a.crtc_rect();
  auto rb = b.crtc_rect();
  return ra.x + static_cast<int64_t>(ra.w) > rb.x && rb.x + static_cast<int64_t>(rb.w) > ra.x &&
         ra.y + static_cast<int64_t>(ra.h) > rb.y && rb.y + static_cast<int64_t>(rb.h) > ra.y;
}

std::vector<std::vector<Layer*>> Allocator::split_independent_groups(std::vector<Layer*>& layers) {
  if (layers.size() <= 1) {
    if (layers.empty()) {
      return {};
    }
    return {layers};
  }

  // Union-find
  std::vector<std::size_t> parent(layers.size());
  std::iota(parent.begin(), parent.end(), static_cast<std::size_t>(0));

  std::function<std::size_t(std::size_t)> find = [&](std::size_t x) -> std::size_t {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };

  auto unite = [&](std::size_t a, std::size_t b) {
    a = find(a);
    b = find(b);
    if (a != b) {
      parent[a] = b;
    }
  };

  // Group overlapping layers
  for (std::size_t i = 0; i < layers.size(); ++i) {
    for (std::size_t j = i + 1; j < layers.size(); ++j) {
      if (layers_intersect(*layers[i], *layers[j])) {
        unite(i, j);
      }
    }
  }

  // Collect groups
  std::unordered_map<std::size_t, std::vector<Layer*>> group_map;
  for (std::size_t i = 0; i < layers.size(); ++i) {
    group_map[find(i)].push_back(layers[i]);
  }

  std::vector<std::vector<Layer*>> result;
  result.reserve(group_map.size());
  for (auto& [_, group] : group_map) {
    result.push_back(std::move(group));
  }
  return result;
}

// ── Test commit helpers ───────────────────────────────────────

bool Allocator::try_test_commit(const std::unordered_map<uint32_t, Layer*>& assignment,
                                [[maybe_unused]] Output& output, AtomicRequest& req,
                                uint32_t flags) {
  if (test_commits_this_frame_ >= max_test_commits_) {
    return false;
  }

  // Build atomic request from assignment
  for (const auto& [plane_id, layer] : assignment) {
    auto result = apply_layer_to_plane(*layer, plane_id, req);
    if (!result.has_value()) {
      return false;
    }
  }

  ++test_commits_this_frame_;
  auto result = req.test(flags);

  // Record in failure cache
  for (const auto& [plane_id, layer] : assignment) {
    failure_cache_.record(plane_id, layer->property_hash(), result.has_value());
  }

  return result.has_value();
}

drm::expected<void, std::error_code> Allocator::apply_layer_to_plane(const Layer& layer,
                                                                     uint32_t plane_id,
                                                                     AtomicRequest& req) {
  for (const auto& [name, value] : layer.properties()) {
    auto prop_id = prop_store_.property_id(plane_id, name);
    if (prop_id.has_value()) {
      auto result = req.add_property(plane_id, *prop_id, value);
      if (!result.has_value()) {
        return result;
      }
    }
    // If property not found in store, skip silently — not all layers
    // set properties that exist on all planes.
  }
  return {};
}

// ── Backtracking search ───────────────────────────────────────

bool Allocator::backtrack(std::vector<Layer*>& layers,
                          const std::vector<const PlaneCapabilities*>& planes,
                          std::unordered_map<uint32_t, Layer*>& assignment, std::size_t depth,
                          std::size_t best_so_far, AtomicRequest& req, uint32_t flags,
                          uint32_t crtc_index) {
  if (depth >= layers.size()) {
    return true;
  }
  if (test_commits_this_frame_ >= max_test_commits_) {
    return false;
  }

  Layer* layer = layers[depth];

  // Skip force-composited layers
  if (layer->force_composited_) {
    layer->needs_composition_ = true;
    return backtrack(layers, planes, assignment, depth + 1, best_so_far, req, flags, crtc_index);
  }

  // §13.1 Check upper bound
  auto remaining = drm::span<Layer* const>(layers).subspan(depth);
  int const bound = static_upper_bound(remaining, planes, crtc_index);
  if (assignment.size() + static_cast<std::size_t>(bound) <= best_so_far) {
    return false;  // Can't beat current best
  }

  // Try assigning this layer to each compatible plane
  for (const auto* plane : planes) {
    if (assignment.count(plane->id) != 0) {
      continue;
    }
    if (!plane_statically_compatible(*plane, *layer, crtc_index)) {
      continue;
    }

    // Check failure cache
    auto cached = failure_cache_.lookup(plane->id, layer->property_hash());
    if (cached.has_value() && !*cached) {
      continue;
    }

    assignment[plane->id] = layer;
    layer->assigned_plane_ = plane->id;

    if (backtrack(layers, planes, assignment, depth + 1, best_so_far, req, flags, crtc_index)) {
      return true;
    }

    // Undo
    assignment.erase(plane->id);
    layer->assigned_plane_ = std::nullopt;
  }

  // This layer couldn't be assigned — mark for composition
  layer->needs_composition_ = true;
  return backtrack(layers, planes, assignment, depth + 1, best_so_far, req, flags, crtc_index);
}

}  // namespace drm::planes
