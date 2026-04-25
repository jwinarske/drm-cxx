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

#include <drm_mode.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
bool alloc_debug() {
  static const bool enabled = std::getenv("DRM_ALLOC_DEBUG") != nullptr;
  return enabled;
}

const char* plane_type_name(drm::planes::DRMPlaneType t) {
  switch (t) {
    case drm::planes::DRMPlaneType::PRIMARY:
      return "PRIMARY";
    case drm::planes::DRMPlaneType::OVERLAY:
      return "OVERLAY";
    case drm::planes::DRMPlaneType::CURSOR:
      return "CURSOR";
  }
  return "?";
}
}  // namespace

namespace drm::planes {
// ── TestCache ──────────────────────────────────────────────────

std::optional<bool> TestCache::lookup(uint32_t plane_id, std::size_t prop_hash) const {
  const auto it = cache_.find({plane_id, prop_hash});
  if (it == cache_.end()) {
    return std::nullopt;
  }
  return it->second.passed;
}

void TestCache::record(uint32_t plane_id, std::size_t prop_hash, const bool passed) {
  auto& entry = cache_.try_emplace(std::make_pair(plane_id, prop_hash)).first->second;
  entry.passed = passed;
  entry.hits++;
}

std::size_t TestCache::hit_count(uint32_t plane_id, std::size_t prop_hash) const {
  const auto it = cache_.find({plane_id, prop_hash});
  if (it == cache_.end()) {
    return 0;
  }
  return it->second.hits;
}

void TestCache::clear() noexcept {
  cache_.clear();
}

// ── Allocator ──────────────────────────────────────────────────

Allocator::Allocator(const Device& dev, PlaneRegistry& registry) : dev_(dev), registry_(registry) {
  // Cache each plane's property ids so apply_layer_to_plane can translate
  // property names ("FB_ID", "CRTC_ID", "zpos", ...) into ids when building
  // the atomic request. Without this, every property_id() lookup misses
  // and the request ships with zero plane property changes — the kernel
  // then accepts the commit but keeps whatever fb was already on the
  // plane (e.g. the fbcon console buffer), producing a "modeset-applied
  // but nothing rendered" blank-screen symptom.
  for (const auto& plane : registry_.all()) {
    (void)prop_store_.cache_properties(dev_.fd(), plane.id, DRM_MODE_OBJECT_PLANE);
  }
}

void Allocator::set_max_test_commits(const std::size_t max) noexcept {
  max_test_commits_ = max;
}

void Allocator::set_test_preparer(TestPreparer preparer) {
  test_preparer_ = std::move(preparer);
}

// ── Main entry point (§13.3 warm-start logic) ──────────────────

drm::expected<std::size_t, std::error_code> Allocator::apply(Output& output, AtomicRequest& req,
                                                             const uint32_t commit_flags) {
  test_commits_this_frame_ = 0;

  // Reset layer assignment state
  for (auto* layer : output.layers()) {
    layer->needs_composition_ = false;
    layer->assigned_plane_ = std::nullopt;
  }

  // Determine CRTC index once. Passed to every path that has to filter
  // planes by CRTC (warm-start, full search, try_test_commit, stale-plane
  // disables).
  uint32_t crtc_index = 0;
  const auto all_planes = registry_.all();
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

  // Fast path: nothing changed since last frame
  if (!output.any_layer_dirty() && previous_allocation_valid_) {
    return apply_previous_allocation(output, req, commit_flags, crtc_index);
  }

  // Warm-start: try previous allocation first (one test commit)
  if (previous_allocation_valid_) {
    if (const auto result = apply_previous_allocation(output, req, commit_flags, crtc_index);
        result.has_value()) {
      output.mark_clean();
      return result;
    }
  }

  // Full search
  return full_search(output, req, commit_flags, crtc_index);
}

// ── Warm-start from previous frame ────────────────────────────

drm::expected<std::size_t, std::error_code> Allocator::apply_previous_allocation(
    Output& output, AtomicRequest& req, const uint32_t flags, const uint32_t crtc_index) {
  // Validate that all layer pointers from previous allocation still exist
  const auto& current_layers = output.layers();
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
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  if (!try_test_commit(previous_allocation_, flags, crtc_index)) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  // TEST passed → populate the caller's request so the real commit
  // reflects the tested state: disables for dropped planes first, then
  // property writes for planes we're keeping.
  disable_unused_planes(req, crtc_index, previous_allocation_);
  std::size_t assigned = 0;
  for (auto& [plane_id, layer] : previous_allocation_) {
    if (auto r = apply_layer_to_plane(*layer, plane_id, req); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
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

// ── Full search with all improvements ─────────────────────────

drm::expected<std::size_t, std::error_code> Allocator::full_search(Output& output,
                                                                   AtomicRequest& req,
                                                                   const uint32_t flags,
                                                                   const uint32_t crtc_index) {
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
      assignment.insert_or_assign(plane->id, layer);
    }

    // Test the preseed assignment
    if (!assignment.empty()) {
      if (alloc_debug()) {
        std::fprintf(stderr, "[drm-cxx] TEST preseed assignment:\n");
        for (const auto& [pid, lay] : assignment) {
          std::fprintf(stderr, "[drm-cxx]   plane=%u ← layer=%p\n", pid,
                       static_cast<const void*>(lay));
        }
      }
      bool const preseed_ok = try_test_commit(assignment, flags, crtc_index);
      if (alloc_debug()) {
        std::fprintf(stderr, "[drm-cxx] TEST preseed → %s\n", preseed_ok ? "PASS" : "FAIL");
      }
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
          if (auto cached = failure_cache_.lookup(cand.plane->id, cand.layer->property_hash());
              cached.has_value() && !*cached) {
            continue;
          }

          assignment.insert_or_assign(cand.plane->id, cand.layer);
          used_planes.insert_or_assign(cand.plane->id, true);
          assigned_layers.insert_or_assign(cand.layer, true);
        }

        if (alloc_debug()) {
          std::fprintf(stderr, "[drm-cxx] TEST greedy assignment:\n");
          for (const auto& [pid, lay] : assignment) {
            std::fprintf(stderr, "[drm-cxx]   plane=%u ← layer=%p\n", pid,
                         static_cast<const void*>(lay));
          }
        }
        bool const greedy_ok = try_test_commit(assignment, flags, crtc_index);
        if (alloc_debug()) {
          std::fprintf(stderr, "[drm-cxx] TEST greedy → %s\n", greedy_ok ? "PASS" : "FAIL");
        }
        if (!greedy_ok) {
          // Backtrack: remove assignments one by one from the end
          auto assigned_vec =
              std::vector<std::pair<uint32_t, Layer*>>(assignment.begin(), assignment.end());

          // Sort by layer priority (lowest priority dropped first)
          std::sort(assigned_vec.begin(), assigned_vec.end(), [](const auto& a, const auto& b) {
            return layer_priority(*a.second) < layer_priority(*b.second);
          });

          for (auto& [fst, snd] : assigned_vec) {
            assignment.erase(fst);

            if (try_test_commit(assignment, flags, crtc_index)) {
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
      best_assignment.insert_or_assign(plane_id, layer);
      ++total_assigned;
    }

    // Remove used planes from available
    for (auto& [fst, snd] : assignment) {
      const uint32_t plane_id = fst;
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
      return drm::unexpected<std::error_code>(r.error());
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

  // Track every plane we write to this frame so disable_unused_planes
  // below doesn't turn around and clear a plane we just armed.
  auto planes_in_use = best_assignment;

  if (any_composited && (output.composition_layer() != nullptr)) {
    // Find primary plane for this crtc
    for (const auto* plane : registry_.for_crtc(crtc_index)) {
      if (plane->type == DRMPlaneType::PRIMARY && best_assignment.count(plane->id) == 0) {
        output.composition_layer()->assigned_plane_ = plane->id;
        if (auto r = apply_layer_to_plane(*output.composition_layer(), plane->id, req); !r) {
          return drm::unexpected<std::error_code>(r.error());
        }
        planes_in_use.insert_or_assign(plane->id, output.composition_layer());
        break;
      }
    }
  }

  // Explicitly disable every other CRTC-compatible non-cursor plane so
  // the commit doesn't inherit stale FB/CRTC/zpos from the previous
  // frame. Without this, a plane the allocator stopped using keeps
  // scanning out its last FB and continues to contend for zpos with
  // whatever the allocator did pick — blank screen or EINVAL next frame.
  //
  // Skipped on the very first commit: there's no previous frame to
  // inherit from, and an ALLOW_MODESET commit that also disables
  // currently-idle overlays appears to race the PAGE_FLIP_EVENT queue
  // on amdgpu (kernel delivers the commit but no event arrives,
  // wedging the caller's flip_pending). 5bcc2b9a's hand-rolled path
  // never touched idle overlays on the first commit and didn't see
  // this; match that shape here.
  if (previous_allocation_valid_) {
    disable_unused_planes(req, crtc_index, planes_in_use);
  }

  // Save for warm-start next frame
  previous_allocation_ = best_assignment;
  previous_allocation_valid_ = true;

  output.mark_clean();
  return total_assigned;
}

// ── §13.5 Bipartite pre-solve ─────────────────────────────────

std::vector<std::pair<Layer*, const PlaneCapabilities*>> Allocator::bipartite_preseed(
    Output& output, const uint32_t crtc_index) const {
  return bipartite_preseed_group(output.layers(), registry_.for_crtc(crtc_index), crtc_index);
}

// Helper: preseed for a group of layers and available planes
std::vector<std::pair<Layer*, const PlaneCapabilities*>> Allocator::bipartite_preseed_group(
    std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
    const uint32_t crtc_index) const {
  std::vector<std::pair<Layer*, const PlaneCapabilities*>> result;

  if (layers.empty() || planes.empty()) {
    return result;
  }

  BipartiteMatching matching(layers.size(), planes.size());

  for (std::size_t i = 0; i < layers.size(); ++i) {
    for (std::size_t j = 0; j < planes.size(); ++j) {
      if (plane_statically_compatible(*planes.at(j), *layers.at(i), crtc_index)) {
        int const s = score_pair(*planes.at(j), *layers.at(i));
        matching.add_edge(i, j, s);
        if (alloc_debug()) {
          const auto& p = *planes.at(j);
          const auto& lay = *layers.at(i);
          const auto z = lay.property("zpos");
          std::fprintf(stderr,
                       "[drm-cxx] cand: layer=%zu plane=%u (%s) score=%d "
                       "layer_zpos=%lld plane_zpos=[%lld,%lld] is_comp=%d\n",
                       i, p.id, plane_type_name(p.type), s,
                       z.has_value() ? static_cast<long long>(*z) : -1LL,
                       p.zpos_min.has_value() ? static_cast<long long>(*p.zpos_min) : -1LL,
                       p.zpos_max.has_value() ? static_cast<long long>(*p.zpos_max) : -1LL,
                       lay.is_composition_layer() ? 1 : 0);
        }
      } else if (alloc_debug()) {
        std::fprintf(stderr, "[drm-cxx] incompat: layer=%zu plane=%u (%s)\n", i, planes.at(j)->id,
                     plane_type_name(planes.at(j)->type));
      }
    }
  }

  matching.solve();

  for (std::size_t i = 0; i < layers.size(); ++i) {
    auto m = matching.match_for_left(i);
    if (m.has_value()) {
      result.emplace_back(layers.at(i), planes.at(*m));
      if (alloc_debug()) {
        std::fprintf(stderr, "[drm-cxx] preseed: layer=%zu → plane=%u (%s)\n", i, planes.at(*m)->id,
                     plane_type_name(planes.at(*m)->type));
      }
    } else if (alloc_debug()) {
      std::fprintf(stderr, "[drm-cxx] preseed: layer=%zu UNMATCHED\n", i);
    }
  }

  return result;
}

// ── §13.2 Best-first search order ─────────────────────────────

std::vector<CandidatePair> Allocator::rank_candidates(Output& output,
                                                      const uint32_t crtc_index) const {
  return rank_candidates_group(output.layers(), registry_.for_crtc(crtc_index), crtc_index);
}

std::vector<CandidatePair> Allocator::rank_candidates_group(
    const std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
    const uint32_t crtc_index) const {
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
  if (const auto fmt = layer.format(); fmt && plane.supports_format(*fmt)) {
    s += 4;
  }

  // Composition layer strongly prefers primary plane
  if (layer.is_composition_layer() && plane.type == DRMPlaneType::PRIMARY) {
    s += 8;
  }

  // Non-composition layer whose zpos matches primary's minimum slot is
  // explicitly requesting direct scan-out on primary (e.g. the bottommost
  // Flutter layer where the caller pinned zpos = primary.zpos_min).
  // Without this, the OVERLAY bonus below wins and primary stays FB-less,
  // leaving whatever was on primary before (typically the fbcon console
  // fb) on scanout — blank screen / console text visible.
  if (!layer.is_composition_layer() && plane.type == DRMPlaneType::PRIMARY) {
    if (const auto z = layer.property("zpos");
        z.has_value() && plane.zpos_min.has_value() && *z == *plane.zpos_min) {
      s += 10;
    }
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
                                            const uint32_t crtc_index) {
  if (!plane.compatible_with_crtc(crtc_index)) {
    return false;
  }

  if (const auto fmt = layer.format(); fmt && !plane.supports_format(*fmt)) {
    return false;
  }

  if (layer.rotation() != 0 && !plane.supports_rotation) {
    return false;
  }

  if (layer.requires_scaling() && !plane.supports_scaling) {
    return false;
  }

  if (const auto z = layer.property("zpos"); z.has_value()) {
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

int Allocator::static_upper_bound(const drm::span<Layer* const> remaining_layers,
                                  const std::vector<const PlaneCapabilities*>& available_planes,
                                  const uint32_t crtc_index) {
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

  const std::function find = [&](std::size_t x) -> std::size_t {
    while (parent.at(x) != x) {
      parent.at(x) = parent.at(parent.at(x));
      x = parent.at(x);
    }
    return x;
  };

  auto unite = [&](std::size_t a, std::size_t b) {
    a = find(a);
    b = find(b);
    if (a != b) {
      parent.at(a) = b;
    }
  };

  // Group overlapping layers
  for (std::size_t i = 0; i < layers.size(); ++i) {
    for (std::size_t j = i + 1; j < layers.size(); ++j) {
      if (layers_intersect(*layers.at(i), *layers.at(j))) {
        unite(i, j);
      }
    }
  }

  // Collect groups
  std::unordered_map<std::size_t, std::vector<Layer*>> group_map;
  for (std::size_t i = 0; i < layers.size(); ++i) {
    group_map.try_emplace(find(i)).first->second.push_back(layers.at(i));
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
                                const uint32_t flags, const uint32_t crtc_index) {
  if (test_commits_this_frame_ >= max_test_commits_) {
    return false;
  }

  // Build a fresh atomic request for this TEST. Using a local request
  // lets us combine stale-plane disables with the proposed assignment
  // without polluting any caller-owned request; callers apply the
  // winning state separately.
  AtomicRequest test_req(dev_);

  // Caller-supplied decoration (e.g. modeset MODE_ID/ACTIVE on the
  // first commit). A fresh test_req is otherwise plane-only, and on
  // frame 0 the CRTC is still inactive — the kernel then rejects
  // plane-on-inactive-CRTC with EINVAL for every TEST.
  if (test_preparer_) {
    if (auto r = test_preparer_(test_req, flags); !r) {
      if (alloc_debug()) {
        std::fprintf(stderr, "[drm-cxx] test_preparer FAIL: %s (errno=%d)\n",
                     r.error().message().c_str(), r.error().value());
      }
      return false;
    }
  }

  // First, disable every CRTC-compatible non-cursor plane that isn't
  // in this assignment. Atomic TEST merges against the currently
  // committed state, so without these disables the kernel sees the
  // previous frame's FB/CRTC/zpos on planes we've stopped using —
  // which typically contends with the new assignment (same zpos on
  // same CRTC → EINVAL) and hides the real reason TEST is failing.
  disable_unused_planes(test_req, crtc_index, assignment);

  // Apply each assigned layer's properties
  for (const auto& [plane_id, layer] : assignment) {
    if (auto result = apply_layer_to_plane(*layer, plane_id, test_req); !result.has_value()) {
      if (alloc_debug()) {
        std::fprintf(stderr,
                     "[drm-cxx] apply_layer_to_plane FAIL plane=%u layer=%p: %s (errno=%d)\n",
                     plane_id, static_cast<const void*>(layer), result.error().message().c_str(),
                     result.error().value());
      }
      return false;
    }
  }

  ++test_commits_this_frame_;
  const auto result = test_req.test(flags);

  if (!result.has_value() && alloc_debug()) {
    std::fprintf(stderr, "[drm-cxx] atomic TEST FAIL: %s (errno=%d) — assignment:\n",
                 result.error().message().c_str(), result.error().value());
    for (const auto& [plane_id, layer] : assignment) {
      std::fprintf(stderr, "[drm-cxx]   plane=%u ← layer=%p\n", plane_id,
                   static_cast<const void*>(layer));
    }
  }

  // Record in failure cache
  for (const auto& [plane_id, layer] : assignment) {
    failure_cache_.record(plane_id, layer->property_hash(), result.has_value());
  }

  return result.has_value();
}

void Allocator::disable_unused_planes(AtomicRequest& req, const uint32_t crtc_index,
                                      const std::unordered_map<uint32_t, Layer*>& keep) const {
  for (const auto* plane : registry_.for_crtc(crtc_index)) {
    // Cursor planes are owned by a separate code path (the cursor
    // handler) and shouldn't be force-disabled here.
    if (plane->type == DRMPlaneType::CURSOR) {
      continue;
    }
    if (keep.count(plane->id) != 0) {
      continue;
    }
    if (const auto fb_prop = prop_store_.property_id(plane->id, "FB_ID"); fb_prop.has_value()) {
      (void)req.add_property(plane->id, *fb_prop, 0);
    }
    if (const auto crtc_prop = prop_store_.property_id(plane->id, "CRTC_ID");
        crtc_prop.has_value()) {
      (void)req.add_property(plane->id, *crtc_prop, 0);
    }
  }
}

drm::expected<void, std::error_code> Allocator::apply_layer_to_plane(const Layer& layer,
                                                                     const uint32_t plane_id,
                                                                     AtomicRequest& req) const {
  for (const auto& [name, value] : layer.properties()) {
    auto prop_id = prop_store_.property_id(plane_id, name);
    if (!prop_id.has_value()) {
      // Property not advertised on this plane — not all layers set
      // properties that exist on every plane. Skip silently.
      continue;
    }
    // Immutable properties (e.g. amdgpu pins PRIMARY zpos at 2) are
    // rejected by the atomic uapi with EINVAL regardless of value —
    // see drm_atomic_set_property in drm_atomic_uapi.c. Scene lowering
    // may still populate them as scoring hints (score_pair reads the
    // same property map), so filter them out of the atomic write path
    // rather than stripping them upstream and losing the hint.
    if (prop_store_.is_immutable(plane_id, name).value_or(false)) {
      continue;
    }
    if (auto result = req.add_property(plane_id, *prop_id, value); !result.has_value()) {
      return result;
    }
  }
  return {};
}

// ── Backtracking search ───────────────────────────────────────

bool Allocator::backtrack(std::vector<Layer*>& layers,
                          const std::vector<const PlaneCapabilities*>& planes,
                          std::unordered_map<uint32_t, Layer*>& assignment, const std::size_t depth,
                          const std::size_t best_so_far, AtomicRequest& req, const uint32_t flags,
                          uint32_t crtc_index) {
  if (depth >= layers.size()) {
    return true;
  }
  if (test_commits_this_frame_ >= max_test_commits_) {
    return false;
  }

  Layer* layer = layers.at(depth);

  // Skip force-composited layers
  if (layer->force_composited_) {
    layer->needs_composition_ = true;
    return backtrack(layers, planes, assignment, depth + 1, best_so_far, req, flags, crtc_index);
  }

  // §13.1 Check upper bound
  const auto remaining = drm::span<Layer* const>(layers).subspan(depth);
  if (int const bound = static_upper_bound(remaining, planes, crtc_index);
      assignment.size() + static_cast<std::size_t>(bound) <= best_so_far) {
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
    if (auto cached = failure_cache_.lookup(plane->id, layer->property_hash());
        cached.has_value() && !*cached) {
      continue;
    }

    assignment.insert_or_assign(plane->id, layer);
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
