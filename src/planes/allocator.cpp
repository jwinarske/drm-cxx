// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "allocator.hpp"

#include "../core/device.hpp"
#include "../log.hpp"
#include "../modeset/atomic.hpp"
#include "planes/layer.hpp"
#include "planes/layer_groups.hpp"
#include "planes/multirect.hpp"
#include "planes/output.hpp"
#include "planes/plane_registry.hpp"
#include "planes/zpos_order.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
bool alloc_debug() {
  static const bool enabled = std::getenv("DRM_ALLOC_DEBUG") != nullptr;
  return enabled;
}

// DRM_ALLOC_DEBUG's own gate is this channel's threshold, so the line goes
// to drm::log's sink without the global level getting a second veto — see
// drm::detail::log_channel. Routed rather than printed so it still arrives
// on targets whose stderr goes nowhere.
template <typename... Args>
void alloc_log(drm::format_string<Args...> fmt, Args&&... args) {
  if (!alloc_debug()) {
    return;
  }
  drm::detail::log_channel(drm::LogLevel::Debug, fmt, std::forward<Args>(args)...);
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

namespace {

// The stacked zpos for `plane_id`, if stacked_zpos() renumbered it.
std::optional<uint64_t> zpos_in(const std::vector<std::pair<uint32_t, uint64_t>>& stack,
                                const uint32_t plane_id) {
  for (const auto& [id, z] : stack) {
    if (id == plane_id) {
      return z;
    }
  }
  return std::nullopt;
}

}  // namespace
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
  if (!passed) {
    entry.failures++;
  }
}

std::size_t TestCache::failure_count(uint32_t plane_id, std::size_t prop_hash) const {
  const auto it = cache_.find({plane_id, prop_hash});
  if (it == cache_.end() || it->second.passed) {
    return 0;
  }
  return it->second.failures;
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

void Allocator::forget_layer(const Layer* layer) noexcept {
  if (layer == nullptr) {
    return;
  }
  // Null the layer pointer in last_committed_ but keep the properties.
  // disable_unused_planes uses the entry's FB_ID to decide whether a
  // plane is already off; fully erasing here would make it skip the
  // FB_ID=0 / CRTC_ID=0 emission for the plane this layer was on,
  // leaving the kernel with stale attachment state. Keeping the
  // snapshot lets disable_unused_planes emit the disable on the next
  // commit, and apply_layer_to_plane_real still triggers a full
  // property write for any future layer assigned here because nullptr
  // never matches a live layer pointer.
  for (auto& [plane_id, snapshot] : last_committed_) {
    if (snapshot.layer == layer) {
      snapshot.layer = nullptr;
    }
  }
  for (auto it = previous_allocation_.begin(); it != previous_allocation_.end();) {
    if (it->second == layer) {
      it = previous_allocation_.erase(it);
    } else {
      ++it;
    }
  }
  if (previous_allocation_.empty()) {
    previous_allocation_valid_ = false;
  }
}

// ── Main entry point (§13.3 warm-start logic) ──────────────────

drm::expected<std::size_t, std::error_code> Allocator::apply(
    Output& output, AtomicRequest& req, const uint32_t commit_flags,
    drm::span<const uint32_t> external_reserved, const bool test_only) {
  test_commits_this_frame_ = 0;
  // Diagnostics are per-apply; reset before any property-writing path
  // can run. apply_layer_to_plane_real and disable_unused_planes both
  // bump the same counters.
  diagnostics_ = {};
  // Owning copy for the duration of this apply() — disable_unused_planes
  // consumes it. Cleared on every exit path so a later apply() call
  // never picks up stale state from a previous frame. Copy rather than
  // span so future refactors that move apply() across thread / suspend
  // boundaries can't see the caller's storage reallocate underneath us.
  external_reserved_.assign(external_reserved.begin(), external_reserved.end());
  canvas_plane_.reset();
  struct ResetReserved {
    Allocator* self;
    ~ResetReserved() { self->external_reserved_.clear(); }
  };
  const ResetReserved reset_reserved{this};

  // A "new" layer is one in the scene this frame that wasn't placed by
  // the previous frame — i.e., not represented as a value in
  // previous_allocation_. apply_previous_allocation is structurally
  // unable to place such a layer: it iterates previous_allocation_ to
  // emit property writes, then dumps everything else through
  // needs_composition_. That's correct when no layer was added (the
  // composition path is genuine fallback) but it's a trap when a fresh
  // layer arrives in steady state — warm-start succeeds with the old
  // set, the new layer is force-composited, and previous_allocation_
  // never grows so the same fate hits every subsequent frame. Detect
  // it here and force full_search so the new layer gets a real shot at
  // a plane.
  bool has_new_layer = false;
  bool any_composited = false;
  if (previous_allocation_valid_) {
    // Build the previous-allocation membership set once, then probe
    // it per current layer. Replaces an O(current × previous) scan
    // with one O(previous) build + O(current) probes.
    std::unordered_set<const Layer*> prev_set;
    prev_set.reserve(previous_allocation_.size());
    for (const auto& [plane_id, prev_layer] : previous_allocation_) {
      prev_set.insert(prev_layer);
    }
    for (const auto* layer : output.layers()) {
      if (layer->is_composition_layer() || layer->is_externally_bound() || layer->is_pinned()) {
        continue;
      }
      // Composited last frame (the flag still holds that verdict here) is
      // not new: warm-start composites it again. Counting it as new forced a
      // full search every frame whenever composition was active.
      if (prev_set.count(layer) == 0 && !layer->needs_composition_) {
        has_new_layer = true;
        break;
      }
      any_composited = any_composited || layer->needs_composition_;
    }
  }
  // A removal frees planes a composited layer may now fit; warm-start would
  // keep it on the canvas (#341). Search once after any shrink that leaves
  // composition running.
  const std::size_t layer_count = output.layers().size();
  const bool shrank = layer_count < committed_layer_count_;
  if (!test_only) {
    committed_layer_count_ = layer_count;
  }
  const bool force_search = has_new_layer || (shrank && any_composited);

  // Reset layer assignment state. Same pass populates the per-apply
  // current-layers set used by has_new_layer / apply_previous_allocation
  // below — O(1) membership instead of two O(N×M) nested-loop scans.
  scratch_current_set_.clear();
  for (auto* layer : output.layers()) {
    layer->needs_composition_ = false;
    // A pinned layer's plane is set by the scene before apply(); it is part of
    // the stack stacked_zpos() numbers.
    if (!layer->is_pinned()) {
      layer->assigned_plane_ = std::nullopt;
    }
    scratch_current_set_.insert(layer);
  }

  // Determine CRTC index once. Passed to every path that has to filter
  // planes by CRTC (warm-start, full search, try_test_commit, stale-plane
  // disables). This MUST be the index of the Output's actual CRTC in
  // drmModeRes::crtcs[] — that bit position is what plane.possible_crtcs
  // is tested against. The old "first index with any compatible plane"
  // heuristic broke on multi-CRTC hardware where the target CRTC isn't
  // the lowest populated one: e.g. RPi5 vc4 has the chosen CRTC at index
  // 2 (primary possible_crtcs=0x4) but its overlays span 0xe (indices
  // 1-3), so the heuristic picked index 1 — a *different* CRTC's plane
  // set, whose PRIMARY then EINVALs against the real CRTC every frame.
  const uint32_t target_crtc = output.crtc_id();
  if (!cached_crtc_index_.has_value() || cached_crtc_index_id_ != target_crtc) {
    uint32_t resolved_index = 0;
    bool resolved = false;
    std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> res(drmModeGetResources(dev_.fd()),
                                                                     drmModeFreeResources);
    if (res) {
      for (int i = 0; i < res->count_crtcs; ++i) {
        if (res->crtcs[i] == target_crtc) {
          resolved_index = static_cast<uint32_t>(i);
          resolved = true;
          break;
        }
      }
    }
    if (!resolved) {
      // Fall back to the legacy first-compatible-index heuristic only
      // when the CRTC can't be located (e.g. a synthetic registry built
      // via from_capabilities() in tests, with no real drmModeRes).
      const auto all_planes = registry_.all();
      for (uint32_t idx = 0; idx < 32; ++idx) {
        const bool found =
            std::any_of(all_planes.begin(), all_planes.end(),
                        [idx](const PlaneCapabilities& p) { return p.compatible_with_crtc(idx); });
        if (found) {
          resolved_index = idx;
          break;
        }
      }
    }
    cached_crtc_index_ = resolved_index;
    cached_crtc_index_id_ = target_crtc;
  }
  const uint32_t crtc_index = *cached_crtc_index_;
  if (last_committed_.empty() && !output.layers().empty()) {
    read_foreign_lit(crtc_index, target_crtc);
  }

  // FB-only fast path: only content (FB_ID / damage / fence) changed on already-
  // placed layers — geometry, format, and modifier are byte-identical to what
  // the kernel last accepted (property_hash match). Reuse the cached allocation
  // and skip the redundant TEST_ONLY; the real commit's atomic_check stays the
  // arbiter, and a rejection invalidates the cache (see finalize_frame). Real
  // commits only — an explicit test() must still probe.
  if (!test_only && previous_allocation_valid_ && !force_search && is_fb_only_frame()) {
    auto result = apply_previous_allocation(output, req, commit_flags, crtc_index,
                                            /*test_only=*/false, /*skip_test=*/true);
    if (result.has_value()) {
      diagnostics_.fb_delta_fast_path = true;
      output.mark_clean();
      return result;
    }
    if (result.error() == std::errc::permission_denied) {
      return result;
    }
    // Anything else (e.g. reserved-plane overlap): fall through to a tested path.
  }

  // Fast path: nothing changed since last frame
  if (!output.any_layer_dirty() && previous_allocation_valid_ && !force_search) {
    auto result = apply_previous_allocation(output, req, commit_flags, crtc_index, test_only);
    if (result.has_value() || result.error() == std::errc::permission_denied) {
      // Success or master loss: return directly. EACCES has to short-
      // circuit here — every TEST that follows would also fail on the
      // dead master, and full_search's silent-failure semantics would
      // hide that from the caller (eventual req.commit() does surface
      // it, but only after a string of doomed test commits and a
      // composition pass).
      return result;
    }
    // Other test failures fall through to full_search, just like the
    // warm-start path below.
  }

  // Warm-start: try previous allocation first (one test commit). Skipped
  // when a new layer is present or a shrink left composition running — see
  // above.
  if (previous_allocation_valid_ && !force_search) {
    auto result = apply_previous_allocation(output, req, commit_flags, crtc_index, test_only);
    if (result.has_value()) {
      output.mark_clean();
      return result;
    }
    if (result.error() == std::errc::permission_denied) {
      return result;
    }
  }

  // Full search
  return full_search(output, req, commit_flags, crtc_index, test_only);
}

bool Allocator::is_fb_only_frame() const {
  // Every plane we committed last frame must still map to the same layer, and
  // that layer's current property_hash() must match what we recorded at commit.
  // property_hash() excludes FB_ID and IN_FENCE_FD, so a match proves only
  // content changed — geometry, format, and modifier are identical to the
  // assignment the kernel already accepted. Any placement / format / modifier
  // edit, or a layer that vanished this frame, defeats the fast path.
  if (previous_allocation_.empty()) {
    return false;
  }
  return std::all_of(previous_allocation_.begin(), previous_allocation_.end(),
                     [this](const auto& entry) {
                       const auto& [plane_id, layer] = entry;
                       if (scratch_current_set_.count(layer) == 0) {
                         return false;  // a previously-placed layer is gone this frame
                       }
                       const auto it = last_committed_.find(plane_id);
                       // Same plane/layer baseline, and geometry/format/modifier unchanged
                       // since the kernel accepted it (property_hash excludes FB_ID/fence).
                       return it != last_committed_.end() && it->second.layer == layer &&
                              it->second.hash == layer->property_hash();
                     });
}

// ── Warm-start from previous frame ────────────────────────────

drm::expected<std::size_t, std::error_code> Allocator::apply_previous_allocation(
    Output& output, AtomicRequest& req, const uint32_t flags, const uint32_t crtc_index,
    const bool test_only, const bool skip_test) {
  // Bail to full_search when the caller's external_reserved set
  // overlaps with our remembered allocation. The previous frame's
  // assignment placed a layer where the caller now wants the canvas
  // (or some other reserved use); we can't honor both, and the
  // simplest path is to re-search from scratch.
  for (const auto& [plane_id, layer] : previous_allocation_) {
    for (const auto reserved : external_reserved_) {
      if (plane_id == reserved) {
        return drm::unexpected<std::error_code>(
            std::make_error_code(std::errc::resource_unavailable_try_again));
      }
    }
  }

  // Validate that all layer pointers from previous allocation still
  // exist. scratch_current_set_ was populated at the top of apply()
  // — O(1) membership check per stale-entry probe replaces the
  // previous O(prev × current) nested-loop scan.
  for (auto it = previous_allocation_.begin(); it != previous_allocation_.end();) {
    if (scratch_current_set_.count(it->second) == 0) {
      it = previous_allocation_.erase(it);
    } else {
      ++it;
    }
  }

  if (previous_allocation_.empty()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  // A zpos change since last frame can invert a stack that involves a
  // fixed-slot plane; TEST would not notice, so re-search instead.
  if (!stacking_consistent(previous_allocation_) || !multirect_complete(previous_allocation_)) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }
  // Plane-order CRTC: a zpos change can move a layer across the canvas or
  // another plane, which nothing but this check notices.
  const bool by_plane_id = stacks_by_plane_id(crtc_index);
  if (by_plane_id &&
      !plane_order_consistent(output, previous_allocation_, previous_canvas_plane_)) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  // Zpos CRTC: a move or restack can leave a placed layer on the wrong side of
  // the one canvas; re-search so the conflict is resolved.
  if (!by_plane_id && !canvas_bounds(previous_allocation_).feasible()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  // FB-only fast path bypasses re-validation: the caller proved via
  // is_fb_only_frame() that geometry/format/modifier are unchanged from the
  // last accepted commit, so the cached assignment is still valid. The real
  // commit's atomic_check remains the arbiter (a rejection invalidates the
  // cache — see finalize_frame).
  if (!skip_test) {
    if (auto ec = try_test_commit(previous_allocation_, flags, crtc_index); ec) {
      // EACCES means the kernel revoked our master (libseat pause_cb may
      // not have arrived yet — drmIsMaster lags). Propagate it up so the
      // caller can soft-pause; flattening it to EAGAIN here would hide
      // the real signal. Other failures stay flattened to EAGAIN, which
      // is the established "warm-start didn't fit, fall through to a
      // fresh search" signal at higher layers.
      if (ec == std::errc::permission_denied) {
        return drm::unexpected<std::error_code>(ec);
      }
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }
  }

  // TEST passed → populate the caller's request so the real commit
  // reflects the tested state: disables for dropped planes first, then
  // property writes for planes we're keeping. The warm-start path is
  // the steady-state best case for property minimization: every plane
  // already has the correct layer pointer in last_committed_, so
  // unchanged properties skip the wire entirely.
  disable_unused_planes(req, crtc_index, previous_allocation_, /*track_state=*/true, test_only);
  std::size_t assigned = 0;
  const auto stack = stacked_zpos(previous_allocation_);
  for (auto& [plane_id, layer] : previous_allocation_) {
    if (auto r = apply_layer_to_plane_real(*layer, plane_id, req, test_only,
                                           zpos_in(stack.renumbered, plane_id));
        !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    layer->assigned_plane_ = plane_id;
    layer->needs_composition_ = false;
    ++assigned;
  }
  // Mark unassigned layers as needing composition
  for (auto* layer : output.layers()) {
    if (!layer->assigned_plane_.has_value() && !layer->is_composition_layer() &&
        !layer->is_externally_bound() && !layer->is_pinned()) {
      layer->needs_composition_ = true;
    }
  }
  if (by_plane_id) {
    canvas_plane_ = previous_canvas_plane_;
  }
  record_stack(stack, std::any_of(output.layers().begin(), output.layers().end(),
                                  [](const Layer* l) { return l->needs_composition(); }));
  output.mark_clean();
  return assigned;
}

// ── Full search with all improvements ─────────────────────────

drm::expected<std::size_t, std::error_code> Allocator::full_search(Output& output,
                                                                   AtomicRequest& req,
                                                                   const uint32_t flags,
                                                                   const uint32_t crtc_index,
                                                                   const bool test_only) {
  // Sampled before this frame's writes record anything (see the disable pass).
  const bool committed_before = !last_committed_.empty();
  output.sort_layers_by_zpos();

  // Externally-bound layers (e.g. EGL stream sources whose plane is
  // owned out-of-band) must not feed into the bipartite match — they
  // already have a plane and the scene writes their properties
  // directly. Strip them once here so split_independent_groups,
  // place_group, and the per-group TEST commits don't see them.
  // Reused scratch — capacity carries forward across frames, so a
  // steady-state caller pays one allocation total instead of one
  // per apply().
  scratch_placeable_.clear();
  scratch_placeable_.reserve(output.layers().size());
  for (auto* l : output.layers()) {
    if (l->is_externally_bound() || l->is_transient_composited() || l->is_pinned()) {
      continue;
    }
    scratch_placeable_.push_back(l);
  }

  // §13.7 Spatial intersection splitting
  auto groups = split_independent_groups(scratch_placeable_);

  auto available_planes = registry_.for_crtc(crtc_index);
  // Externally reserved planes are off-limits for placement *and*
  // for the disable-unused pass. The caller (e.g. LayerScene's
  // compose_unassigned) will arm them itself; if we let the
  // bipartite preseed grab one, the caller would either fight us
  // for the slot or its canvas would have nowhere to land.
  if (!external_reserved_.empty()) {
    available_planes.erase(
        std::remove_if(available_planes.begin(), available_planes.end(),
                       [this](const PlaneCapabilities* p) {
                         return std::any_of(external_reserved_.begin(), external_reserved_.end(),
                                            [p](const std::uint32_t pid) { return pid == p->id; });
                       }),
        available_planes.end());
  }

  PlaneAssignment best_assignment;
  std::size_t total_assigned = 0;
  const bool by_plane_id = stacks_by_plane_id(crtc_index);

  // Scene-wide plane pool, captured before the per-group loop shrinks
  // available_planes. The partial-fallback retry below needs the
  // unshrunk pool — its whole point is to consider plane assignments
  // the per-group pass couldn't reach.
  const auto all_available_planes = available_planes;

  if (by_plane_id) {
    // Stacking is fixed by plane id: one ordered pass over the whole scene.
    // The spatial split does not apply (the canvas spans every group).
    // Transient-composited layers land on the canvas too, so they count.
    groups.clear();
    std::vector<Layer*> ordered;
    ordered.reserve(output.layers().size());
    for (auto* l : output.layers()) {
      if (!l->is_composition_layer() && !l->is_externally_bound() && !l->is_pinned()) {
        ordered.push_back(l);
      }
    }
    best_assignment = place_in_plane_order(
        ordered, available_planes, output.composition_layer() != nullptr, flags, crtc_index);
    total_assigned = best_assignment.size();
  }

  for (auto& group : groups) {
    auto assignment = place_group(group, available_planes, flags, crtc_index);

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

  // ── Best-partial fallback ─────────────────────────────────────
  // Per-group placement returned nothing, but some layers may still
  // be placeable when considered scene-wide. The §13.7 split is an
  // optimization for non-overlapping layouts; when every per-group
  // attempt drops to empty under TEST, fall back to a scene-wide
  // pass that drops the most-constrained layer (fewest statically
  // compatible planes) on each retry. The dropped layers are routed
  // through composition by the post-loop needs_composition_ pass —
  // exactly the same path a single failed group would have taken.
  if (total_assigned == 0 && !by_plane_id) {
    std::vector<Layer*> placeable;
    placeable.reserve(output.layers().size());
    for (auto* l : output.layers()) {
      if (l->force_composited_ || l->is_transient_composited() || l->is_composition_layer() ||
          l->is_externally_bound() || l->is_pinned()) {
        continue;
      }
      placeable.push_back(l);
    }

    while (!placeable.empty() && test_commits_this_frame_ < max_test_commits_) {
      auto attempt = place_group(placeable, all_available_planes, flags, crtc_index);
      if (!attempt.empty()) {
        alloc_log("[alloc] partial-fallback PASS with {} of {} layer(s)", attempt.size(),
                  placeable.size());
        best_assignment = std::move(attempt);
        total_assigned = best_assignment.size();
        break;
      }
      const Layer* victim = pick_most_constrained(placeable, all_available_planes, crtc_index);
      if (victim == nullptr) {
        break;
      }
      alloc_log("[alloc] partial-fallback drop most-constrained layer={}",
                static_cast<const void*>(victim));
      placeable.erase(std::find(placeable.begin(), placeable.end(), victim));
    }
  }

  // One canvas stacks at one zpos. When a placed layer would have to sit on
  // both sides of it, composite one contiguous zpos run instead; failing that,
  // move the caught layers into the composition and keep the result if the
  // smaller set passes.
  if (!by_plane_id && !canvas_bounds(best_assignment).feasible()) {
    if (auto run = place_around_run(output, best_assignment.size(), all_available_planes, flags,
                                    crtc_index);
        run.has_value()) {
      best_assignment = std::move(*run);
    } else {
      auto resolved = best_assignment;
      if (resolve_canvas_bounds(resolved) &&
          (resolved.empty() || (test_commits_this_frame_ < max_test_commits_ &&
                                !try_test_commit(resolved, flags, crtc_index)))) {
        best_assignment = std::move(resolved);
      }
    }
    total_assigned = best_assignment.size();
  }

  // Apply the best assignment. Real-commit path → minimization-aware
  // variant; the per-plane snapshot detects layer reassignment versus
  // last frame and forces a full write when it happened, otherwise
  // skips properties whose value is unchanged.
  const auto stack = stacked_zpos(best_assignment);
  for (auto& [plane_id, layer] : best_assignment) {
    layer->assigned_plane_ = plane_id;
    layer->needs_composition_ = false;
    if (auto r = apply_layer_to_plane_real(*layer, plane_id, req, test_only,
                                           zpos_in(stack.renumbered, plane_id));
        !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
  }

  // Mark unassigned layers
  for (auto* layer : output.layers()) {
    if (!layer->assigned_plane_.has_value() && !layer->is_composition_layer() &&
        !layer->is_externally_bound() && !layer->is_pinned()) {
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

  // The plane-order path picked the canvas plane itself (canvas_plane_).
  if (any_composited && (output.composition_layer() != nullptr) && !by_plane_id) {
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
  // this; match that shape here. "First" is nothing committed yet, not a
  // missing warm-start: after the scene's layers are replaced (or the
  // allocation is invalidated) the planes they left armed still need the
  // disable, or they keep their zpos and collide with the new stack. An empty
  // scene keeps its planes: disabling an active CRTC's only PRIMARY is refused
  // (i.MX LCDIF), and the last frame stays up as before. Before anything is
  // committed, only planes another client left lit are disabled: every TEST
  // disables them, so the real commit must too.
  const bool has_scene_layers =
      std::any_of(output.layers().begin(), output.layers().end(),
                  [](const Layer* l) { return !l->is_composition_layer(); });
  if ((committed_before || !foreign_lit_.empty()) && has_scene_layers) {
    disable_unused_planes(req, crtc_index, planes_in_use, /*track_state=*/true, test_only);
  }

  // Save for warm-start next frame. An empty assignment is *not* valid
  // warm-start state: it happens when full_search exhausts its TEST
  // budget without finding any non-empty subset that the kernel
  // accepts (compose_unassigned then rescues every layer onto the
  // canvas). Marking valid_=true in that case poisons the next frame's
  // fast path — apply_previous_allocation hits its empty-map guard and
  // returns EAGAIN, which the fast path propagates straight to the
  // caller (the warm-start path on lines 153-159 falls through to
  // full_search, but the fast path on line 148 does not).
  if (!any_composited) {
    canvas_plane_.reset();
  }
  record_stack(stack, any_composited);
  previous_allocation_ = best_assignment;
  previous_allocation_valid_ = !best_assignment.empty();
  previous_canvas_plane_ = canvas_plane_;

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
    const std::vector<Layer*>& layers, const std::vector<const PlaneCapabilities*>& planes,
    const uint32_t crtc_index) const {
  std::vector<std::pair<Layer*, const PlaneCapabilities*>> result;

  if (layers.empty() || planes.empty()) {
    return result;
  }

  scratch_matching_.reset(layers.size(), planes.size());

  for (std::size_t i = 0; i < layers.size(); ++i) {
    for (std::size_t j = 0; j < planes.size(); ++j) {
      if (plane_statically_compatible(*planes.at(j), *layers.at(i), crtc_index)) {
        int const s = score_pair(*planes.at(j), *layers.at(i));
        scratch_matching_.add_edge(i, j, s);
        if (alloc_debug()) {
          const auto& p = *planes.at(j);
          const auto& lay = *layers.at(i);
          const auto z = lay.property("zpos");
          alloc_log(
              "[alloc] cand: layer={} plane={} ({}) score={} layer_zpos={} "
              "plane_zpos=[{},{}] is_comp={}",
              i, p.id, plane_type_name(p.type), s,
              z.has_value() ? static_cast<long long>(*z) : -1LL,
              p.zpos_min.has_value() ? static_cast<long long>(*p.zpos_min) : -1LL,
              p.zpos_max.has_value() ? static_cast<long long>(*p.zpos_max) : -1LL,
              lay.is_composition_layer() ? 1 : 0);
        }
      } else {
        alloc_log("[alloc] incompat: layer={} plane={} ({})", i, planes.at(j)->id,
                  plane_type_name(planes.at(j)->type));
      }
    }
  }

  scratch_matching_.solve();

  for (std::size_t i = 0; i < layers.size(); ++i) {
    auto m = scratch_matching_.match_for_left(i);
    if (m.has_value()) {
      result.emplace_back(layers.at(i), planes.at(*m));
      alloc_log("[alloc] preseed: layer={} → plane={} ({})", i, planes.at(*m)->id,
                plane_type_name(planes.at(*m)->type));
    } else {
      alloc_log("[alloc] preseed: layer={} UNMATCHED", i);
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

PlaneAssignment Allocator::place_group(const std::vector<Layer*>& layers,
                                       const std::vector<const PlaneCapabilities*>& planes,
                                       const uint32_t flags, const uint32_t crtc_index) {
  // Process layers highest keep-priority first so that, when planes are
  // scarce, the preseed's max-cardinality matching and the greedy pass
  // keep the higher-priority layers and drop the lower-priority ones
  // (the matching itself is weight-agnostic in its drop choice, so the
  // input order is what decides who survives). Stable, so same-priority
  // layers preserve caller order. keep_priority is content-class
  // dominant, with app_priority breaking within-class ties.
  std::vector<Layer*> ordered(layers.begin(), layers.end());
  std::stable_sort(ordered.begin(), ordered.end(), [](const Layer* a, const Layer* b) {
    return keep_priority(*a) > keep_priority(*b);
  });

  // §13.5 Bipartite pre-solve
  auto preseed = bipartite_preseed_group(ordered, planes, crtc_index);

  PlaneAssignment assignment;
  for (auto& [layer, plane] : preseed) {
    assignment.insert_or_assign(plane->id, layer);
  }

  if (assignment.empty()) {
    return assignment;
  }

  if (alloc_debug()) {
    alloc_log("[alloc] TEST preseed assignment:");
    for (const auto& [pid, lay] : assignment) {
      alloc_log("[alloc]   plane={} ← layer={}", pid, static_cast<const void*>(lay));
    }
  }
  // The matching is order-blind; an inverted stack would pass TEST (the
  // kernel accepts it) yet render wrong, so reject it here and go greedy.
  // Likewise a virtual multirect plane matched without its parent.
  const bool preseed_stacks = stacking_consistent(assignment) && multirect_complete(assignment);
  if (!preseed_stacks) {
    alloc_log("[alloc] preseed inverts the zpos stack or orphans a multirect plane → greedy");
  }
  const bool preseed_ok = preseed_stacks && !try_test_commit(assignment, flags, crtc_index);
  alloc_log("[alloc] TEST preseed → {}", preseed_ok ? "PASS" : "FAIL");
  if (preseed_ok) {
    return assignment;
  }

  // §13.2 + §13.1 Greedy from ranked candidates
  assignment.clear();
  auto candidates = rank_candidates_group(ordered, planes, crtc_index);

  std::unordered_map<uint32_t, bool> used_planes;
  std::unordered_map<Layer*, bool> assigned_layers;

  // Two passes: ordinary planes first, then multirect virtual planes, each
  // only once its parent has been taken in the first pass.
  for (int pass = 0; pass < 2; ++pass) {
    for (const auto& cand : candidates) {
      if (cand.plane->multirect_parent.has_value() != (pass == 1)) {
        continue;
      }
      if (used_planes.count(cand.plane->id) != 0) {
        continue;
      }
      if (assigned_layers.count(cand.layer) != 0) {
        continue;
      }
      if (auto cached = failure_cache_.lookup(cand.plane->id, cand.layer->property_hash());
          cached.has_value() && !*cached) {
        continue;
      }
      if (probe_rejected(crtc_index, cand.plane->id, *cand.layer)) {
        continue;
      }
      if (!fits_stacking(assignment, cand.plane->id, *cand.layer) ||
          !fits_multirect(assignment, cand.plane->id)) {
        continue;
      }
      assignment.insert_or_assign(cand.plane->id, cand.layer);
      used_planes.insert_or_assign(cand.plane->id, true);
      assigned_layers.insert_or_assign(cand.layer, true);
    }
  }

  if (alloc_debug()) {
    alloc_log("[alloc] TEST greedy assignment:");
    for (const auto& [pid, lay] : assignment) {
      alloc_log("[alloc]   plane={} ← layer={}", pid, static_cast<const void*>(lay));
    }
  }
  const bool greedy_ok = !try_test_commit(assignment, flags, crtc_index);
  alloc_log("[alloc] TEST greedy → {}", greedy_ok ? "PASS" : "FAIL");
  if (greedy_ok) {
    return assignment;
  }

  // Backtrack: remove assignments one by one (lowest keep-priority
  // first — content class dominates, app_priority breaks within-class ties)
  auto assigned_vec =
      std::vector<std::pair<uint32_t, Layer*>>(assignment.begin(), assignment.end());
  std::sort(assigned_vec.begin(), assigned_vec.end(), [](const auto& a, const auto& b) {
    return keep_priority(*a.second) < keep_priority(*b.second);
  });

  for (auto& [fst, snd] : assigned_vec) {
    if (assignment.count(fst) == 0) {
      continue;  // already dropped as an orphaned multirect child
    }
    assignment.erase(fst);
    // Dropping a multirect parent orphans its virtual plane; drop that too.
    for (const auto& [pid, lay] : assigned_vec) {
      if (const auto* c = caps_of(pid); c != nullptr && c->multirect_parent == fst) {
        assignment.erase(pid);
      }
    }
    if (auto ec = try_test_commit(assignment, flags, crtc_index); !ec) {
      break;
    }
    if (test_commits_this_frame_ >= max_test_commits_) {
      break;
    }
  }

  return assignment;
}

bool Allocator::stacks_by_plane_id(const uint32_t crtc_index) const {
  const auto& planes = registry_.for_crtc(crtc_index);
  return !planes.empty() && std::none_of(planes.begin(), planes.end(), [](const auto* p) {
    return p->type != DRMPlaneType::CURSOR && p->zpos_min.has_value();
  });
}

PlaneAssignment Allocator::place_in_plane_order(const std::vector<Layer*>& layers,
                                                const std::vector<const PlaneCapabilities*>& planes,
                                                const bool with_canvas, const uint32_t flags,
                                                const uint32_t crtc_index) {
  // Planes in stacking order. Left out rather than modeled: a multirect
  // virtual plane (needs its parent armed alongside) and a cursor plane (size
  // and update rules the static check does not see; often the cursor
  // module's).
  std::vector<const PlaneCapabilities*> by_id;
  by_id.reserve(planes.size());
  for (const auto* p : planes) {
    if (!p->multirect_parent.has_value() && p->type != DRMPlaneType::CURSOR) {
      by_id.push_back(p);
    }
  }
  std::sort(by_id.begin(), by_id.end(), [](const auto* a, const auto* b) { return a->id < b->id; });
  const std::size_t n = layers.size();
  const std::size_t m = by_id.size();
  if (n == 0) {
    return {};
  }

  auto fits = [&](std::size_t i, std::size_t j) {
    const auto& plane = *by_id[j];
    const auto& layer = *layers[i];
    if (!plane_statically_compatible(plane, layer, crtc_index) ||
        probe_rejected(crtc_index, plane.id, layer)) {
      return false;
    }
    const auto cached = failure_cache_.lookup(plane.id, layer.property_hash());
    return !cached.has_value() || *cached;
  };
  auto hosts_canvas = [&](std::size_t j) { return !canvas_host_ || canvas_host_(*by_id[j]); };

  // pre[a]: first plane index free above layers [0, a) placed earliest-first
  // (npos: they don't fit). suf[b]: lowest plane index used by layers [b, n)
  // placed latest-first (npos: they don't fit). Greedy fit is optimal for each.
  constexpr auto npos = static_cast<std::size_t>(-1);
  std::vector<std::size_t> pre(n + 1, npos);
  std::vector<std::size_t> pre_plane(n, npos);
  pre[0] = 0;
  for (std::size_t i = 0; i < n && pre[i] != npos; ++i) {
    for (std::size_t j = pre[i]; j < m; ++j) {
      if (fits(i, j)) {
        pre_plane[i] = j;
        pre[i + 1] = j + 1;
        break;
      }
    }
  }
  std::vector<std::size_t> suf(n + 1, npos);
  std::vector<std::size_t> suf_plane(n, npos);
  suf[n] = m;
  for (std::size_t k = n; k > 0 && suf[k] != npos; --k) {
    const std::size_t i = k - 1;
    for (std::size_t j = suf[k]; j > 0; --j) {
      if (fits(i, j - 1)) {
        suf_plane[i] = j - 1;
        suf[i] = j - 1;
        break;
      }
    }
  }

  // Composited run [a, b): every forced layer must be inside it.
  std::size_t first_forced = n;
  std::size_t last_forced = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (layers[i]->force_composited_ || layers[i]->is_transient_composited()) {
      first_forced = std::min(first_forced, i);
      last_forced = i;
    }
  }
  const bool any_forced = first_forced < n;

  struct Choice {
    std::size_t a{0};
    std::size_t b{0};
    std::optional<std::size_t> canvas;
  };
  // Best run with at most `max_placed` layers on planes: most placed, then the
  // lowest total keep_priority composited.
  auto choose = [&](std::size_t max_placed) -> std::optional<Choice> {
    std::optional<Choice> best;
    std::size_t best_placed = 0;
    long long best_cost = 0;
    for (std::size_t a = 0; a <= n; ++a) {
      if (pre[a] == npos) {
        break;
      }
      for (std::size_t b = a; b <= n; ++b) {
        if (suf[b] == npos || (any_forced && (a > first_forced || b <= last_forced))) {
          continue;
        }
        const std::size_t placed = n - (b - a);
        if (placed > max_placed) {
          continue;
        }
        Choice c{a, b, std::nullopt};
        if (a == b) {
          if (pre[a] > suf[b]) {
            continue;
          }
        } else if (with_canvas) {
          // Topmost free canvas host between the two placed halves.
          for (std::size_t j = suf[b]; j > pre[a]; --j) {
            if (hosts_canvas(j - 1)) {
              c.canvas = j - 1;
              break;
            }
          }
          if (!c.canvas.has_value()) {
            continue;
          }
        } else if (pre[a] > suf[b]) {
          continue;
        }
        long long cost = 0;
        for (std::size_t i = a; i < b; ++i) {
          cost += keep_priority(*layers[i]);
        }
        if (!best.has_value() || placed > best_placed ||
            (placed == best_placed && cost < best_cost)) {
          best = c;
          best_placed = placed;
          best_cost = cost;
        }
      }
    }
    return best;
  };

  std::size_t max_placed = n;
  while (true) {
    const auto choice = choose(max_placed);
    if (!choice.has_value()) {
      return {};
    }
    PlaneAssignment assignment;
    for (std::size_t i = 0; i < choice->a; ++i) {
      assignment.insert_or_assign(by_id[pre_plane[i]]->id, layers[i]);
    }
    for (std::size_t i = choice->b; i < n; ++i) {
      assignment.insert_or_assign(by_id[suf_plane[i]]->id, layers[i]);
    }
    canvas_plane_.reset();
    if (choice->canvas.has_value()) {
      canvas_plane_ = by_id[*choice->canvas]->id;
    }
    alloc_log("[alloc] plane-order: {} placed, run [{}, {}) composited, canvas plane {}",
              assignment.size(), choice->a, choice->b, canvas_plane_.value_or(0));
    if (assignment.empty() || !try_test_commit(assignment, flags, crtc_index)) {
      return assignment;
    }
    if (test_commits_this_frame_ >= max_test_commits_) {
      // Out of TESTs: composite everything rather than arm an untested stack.
      const auto all = choose(0);
      canvas_plane_.reset();
      if (all.has_value() && all->canvas.has_value()) {
        canvas_plane_ = by_id[*all->canvas]->id;
      }
      return {};
    }
    max_placed = assignment.size() - 1;
  }
}

bool Allocator::plane_order_consistent(const Output& output, const PlaneAssignment& assignment,
                                       const std::optional<uint32_t> canvas) {
  // Each placeable layer's stacking position: its plane, or the canvas plane.
  std::vector<std::pair<std::uint64_t, std::optional<uint32_t>>> pos;  // (zpos, plane)
  std::vector<bool> on_canvas;
  for (const auto* layer : output.layers()) {
    if (layer->is_composition_layer() || layer->is_externally_bound() || layer->is_pinned()) {
      continue;
    }
    std::optional<uint32_t> plane;
    for (const auto& [pid, l] : assignment) {
      if (l == layer) {
        plane = pid;
        break;
      }
    }
    on_canvas.push_back(!plane.has_value());
    if (!plane.has_value()) {
      plane = canvas;
    }
    pos.emplace_back(layer->property("zpos").value_or(0), plane);
  }
  for (std::size_t x = 0; x < pos.size(); ++x) {
    for (std::size_t y = 0; y < pos.size(); ++y) {
      if (pos[x].first >= pos[y].first) {
        continue;
      }
      const auto px = pos[x].second;
      const auto py = pos[y].second;
      if (!px.has_value() || !py.has_value()) {
        return false;  // composited with no canvas plane to stack it at
      }
      if (*px > *py || (*px == *py && !(on_canvas[x] && on_canvas[y]))) {
        return false;
      }
    }
  }
  return true;
}

const Layer* Allocator::pick_most_constrained(const std::vector<Layer*>& layers,
                                              const std::vector<const PlaneCapabilities*>& planes,
                                              const uint32_t crtc_index) {
  const Layer* worst = nullptr;
  std::size_t worst_count = std::numeric_limits<std::size_t>::max();
  int worst_keep = std::numeric_limits<int>::max();

  for (const auto* layer : layers) {
    std::size_t count = 0;
    for (const auto* plane : planes) {
      if (plane_statically_compatible(*plane, *layer, crtc_index)) {
        ++count;
      }
    }
    const int keep = keep_priority(*layer);
    // Most-constrained first; lowest keep-priority breaks ties (least
    // valuable to keep when constraint counts agree — content class
    // dominates, app_priority breaks within-class ties).
    if (count < worst_count || (count == worst_count && keep < worst_keep)) {
      worst = layer;
      worst_count = count;
      worst_keep = keep;
    }
  }
  return worst;
}

// Power-aware placement bonus by buffer modifier bandwidth class. Exposed (not
// in the anonymous namespace) so the mapping is unit-testable. See score_pair.
int bandwidth_class_bonus(drm::fmt::BandwidthClass cls) noexcept {
  switch (cls) {
    case drm::fmt::BandwidthClass::Compression:
      return 2;  // DCC / AFBC / UBWC — biggest bandwidth + GPU-decompress saving
    case drm::fmt::BandwidthClass::Tiling:
      return 1;  // better DRAM locality, same byte count
    case drm::fmt::BandwidthClass::Linear:
      return 0;
  }
  return 0;
}

int bandwidth_class_bonus(std::uint64_t modifier) noexcept {
  return bandwidth_class_bonus(drm::fmt::classify(drm::fmt::Modifier{modifier}));
}

int cost_bias(std::uint64_t scanout_bytes) noexcept {
  constexpr std::uint64_t k_mb = std::uint64_t{1024} * 1024;
  if (scanout_bytes >= 16U * k_mb) {
    return 3;  // ~4K RGBA and up
  }
  if (scanout_bytes >= 4U * k_mb) {
    return 2;  // ~1080p RGBA
  }
  if (scanout_bytes >= k_mb) {
    return 1;  // small overlay
  }
  return 0;
}

int Allocator::score_pair(const PlaneCapabilities& plane, const Layer& layer) const {
  int s = 0;

  // Prefer matching format + modifier together — a plane that advertises
  // the layer's exact (format, modifier) is strictly preferable to one
  // that supports the format only via the linear-fallback path.
  if (const auto fmt = layer.format();
      fmt &&
      drm::fmt::layer_fits_plane(plane.format_table, *fmt, drm::fmt::Modifier{layer.modifier()})) {
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
    if (const auto z = layer.property("zpos"); z.has_value()) {
      // NOLINTNEXTLINE(bugprone-branch-clone) — same bonus, two platform paths.
      if (plane.zpos_min.has_value() && *z == *plane.zpos_min) {
        s += 10;
      } else if (!plane.zpos_min.has_value() && *z == 0) {
        // Platforms without a kernel-side zpos property (Tegra Orin
        // display, some other SoCs) can't honor per-plane zpos values,
        // so the caller convention is layer.zpos=0 == "bottom-most"
        // (PRIMARY) and any positive zpos == "above primary" (OVERLAY).
        // Mirrors the +10 above for the zpos-aware path.
        s += 10;
      }
    }
  }

  // Non-composition layers prefer overlay planes
  if (!layer.is_composition_layer() && plane.type == DRMPlaneType::OVERLAY) {
    s += 2;
    // Pair with the "zpos=0 means PRIMARY" hack above: when planes have
    // no kernel zpos and the layer requests a positive zpos, the
    // OVERLAY-as-natural-top convention applies, so bump the score
    // enough to win against the bare PRIMARY base score.
    if (const auto z = layer.property("zpos");
        z.has_value() && !plane.zpos_min.has_value() && *z > 0) {
      s += 8;
    }
  }

  // Bandwidth-aware bias (a small tiebreak below the structural scores; the atomic
  // TEST_ONLY during placement stays the correctness arbiter). Two nudges toward
  // keeping the costliest layers on planes when planes are contested:
  //   * class — a compressed/tiled buffer scanned out directly avoids the GPU
  //     decompress a composition pass would force; a LINEAR layer composites cheaply.
  //   * size — a layer that moves more bytes at scanout is the most expensive to
  //     demote to composition, which re-reads the source and the canvas.
  // The class comes from the scanout FB's own modifier (precomputed per plane),
  // never a producer-internal compression state.
  if (const auto fmt = layer.format(); fmt.has_value()) {
    const auto cls = plane.bandwidth_class(layer.modifier());
    s += bandwidth_class_bonus(cls);
    s += cost_bias(drm::fmt::scanout_cost_bytes(layer.width(), layer.height(), *fmt, cls));
  }

  // §13.6 Content-type priority
  s += layer_priority(layer) / 10;

  // Warm-start stability: strongly prefer keeping a layer on the plane it
  // held last frame. The bipartite matcher maximizes cardinality first and
  // uses this score only to order tie-breaks (matching.hpp visits high-score
  // edges first), so this never places fewer layers and never overrides
  // format/zpos/type validity — plane_statically_compatible gates the edges
  // and the TEST_ONLY commit is the correctness arbiter. Without it, any
  // change to the layer *set* (e.g. Flutter inserting a UI backing-store
  // slice while the platform-view overlays stay put) drops warm-start and
  // makes full_search re-solve from scratch, reshuffling already-placed
  // layers onto different planes — every overlay reprograms in one commit,
  // seen as flicker. The bonus is deliberately larger than the sum of the
  // structural terms above so, among equal-cardinality matchings, the prior
  // assignment wins; a genuinely new layer still displaces an old one only
  // when a plane is contested (cardinality forces it).
  constexpr int warm_stability_bonus = 100;
  if (previous_allocation_valid_) {
    if (const auto it = previous_allocation_.find(plane.id);
        it != previous_allocation_.end() && it->second == &layer) {
      s += warm_stability_bonus;
    }
  }

  // Penalize previously failed combinations. Failures only: a hit count that
  // also grew on success made a plane the kernel keeps accepting decay at the
  // same rate as one it keeps rejecting, so the search walked away from the
  // plane that worked. Measured on a display with one usable primary out of
  // five: the accepted plane was tried ten times and then never again while
  // the allocator cycled the seven overlays, all rejected, ~53 times each.
  s -= static_cast<int>(failure_cache_.failure_count(plane.id, layer.property_hash()));

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

int Allocator::keep_priority(const Layer& layer) {
  // app_priority (0..255) occupies the low byte; the content-class
  // priority is shifted above it so it always dominates — app_priority
  // only breaks ties among layers of the same content class.
  return (layer_priority(layer) * 256) + static_cast<int>(layer.app_priority());
}

// ── §13.1 Static compatibility ────────────────────────────────

bool Allocator::plane_statically_compatible(const PlaneCapabilities& plane, const Layer& layer,
                                            const uint32_t crtc_index) {
  // Force-composited layers are statically incompatible with every
  // plane: the caller (test rig, debug overlay, EGL Streams workaround)
  // has explicitly asked for the composition fallback, and short-
  // circuiting here propagates that decision through every downstream
  // path (bipartite preseed, candidate ranking, recursive backtrack)
  // without each having to re-check the flag.
  if (layer.force_composited_ || layer.is_transient_composited()) {
    return false;
  }
  if (!plane.compatible_with_crtc(crtc_index)) {
    return false;
  }

  const auto fmt = layer.format();
  if (!fmt) {
    // No format means the layer has no buffer this commit — typically a
    // LayerScene EAGAIN-skip where lower_layer() was never called. The
    // permissive interpretation ("no format = compatible with everything")
    // would phantom-place such a layer on a plane: it'd be counted in
    // *assigned but apply_layer_to_plane_real walks an empty property
    // map, so no kernel state changes. The accounting then reports a
    // layer as both assigned and skipped_no_frame, breaking the
    // CommitReport invariant.
    return false;
  }
  // Honor the layer's modifier — AFBC, DCC, and vendor tilings only land
  // on planes whose IN_FORMATS blob explicitly advertises the
  // (format, modifier) pair. Layers that don't tag a modifier fall
  // through the LINEAR/INVALID equivalence path inside layer_fits_plane.
  if (!drm::fmt::layer_fits_plane(plane.format_table, *fmt, drm::fmt::Modifier{layer.modifier()})) {
    return false;
  }

  // The plane must expose every requested rotation/reflect bit, not merely
  // have a rotation property: a plane can advertise rotation yet implement
  // only 0/180 or reflect and would silently drop a 90/270 request.
  if (layer.rotation() != 0 && (plane.rotation_bits & layer.rotation()) != layer.rotation()) {
    return false;
  }

  if (layer.requires_scaling() && !plane.supports_scaling) {
    return false;
  }

  // No zpos range check: the requested value is not what gets written. The
  // allocator writes stack_zpos's dense numbering (zpos_order.hpp), which fits
  // each plane's range, so a layer asking zpos 3 can sit on a [0, 1] plane
  // (tidss). When no dense numbering fits, the requested values are written
  // and the TEST decides. Stacking order is checked per assignment
  // (fits_stacking).

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
  if (layers.empty()) {
    return {};
  }
  // Highest keep_priority group first, so a contested plane goes to the group
  // that matters most (planes/layer_groups.hpp).
  return detail::independent_groups(
      layers, [](const Layer* a, const Layer* b) { return layers_intersect(*a, *b); },
      [](const Layer* l) { return keep_priority(*l); });
}

// ── Test commit helpers ───────────────────────────────────────

bool Allocator::probe_rejected(const uint32_t crtc_index, const uint32_t plane_id,
                               const Layer& layer) const {
  const auto fourcc = layer.format();
  if (!fourcc.has_value()) {
    return false;
  }
  return probe_cache_.lookup(crtc_index, plane_id, *fourcc, drm::fmt::Modifier{layer.modifier()}) ==
         drm::fmt::ModifierProbeCache::Verdict::Rejected;
}

std::error_code Allocator::try_test_commit(const PlaneAssignment& assignment, const uint32_t flags,
                                           const uint32_t crtc_index) {
  if (test_commits_this_frame_ >= max_test_commits_) {
    return std::make_error_code(std::errc::resource_unavailable_try_again);
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
      alloc_log("[alloc] test_preparer FAIL: {} (errno={})", r.error().message(),
                r.error().value());
      return r.error();
    }
  }

  // First, disable every CRTC-compatible non-cursor plane that isn't
  // in this assignment. Atomic TEST merges against the currently
  // committed state, so without these disables the kernel sees the
  // previous frame's FB/CRTC/zpos on planes we've stopped using —
  // which typically contends with the new assignment (same zpos on
  // same CRTC → EINVAL) and hides the real reason TEST is failing.
  // This internal TEST never reaches the caller, so test_only here is
  // moot — track_state=false already prevents any cache writes.
  disable_unused_planes(test_req, crtc_index, assignment, /*track_state=*/false,
                        /*test_only=*/true);

  // Apply each assigned layer's properties
  const auto stack = stacked_zpos(assignment);
  for (const auto& [plane_id, layer] : assignment) {
    if (auto result =
            apply_layer_to_plane(*layer, plane_id, test_req, zpos_in(stack.renumbered, plane_id));
        !result.has_value()) {
      alloc_log("[alloc] apply_layer_to_plane FAIL plane={} layer={}: {} (errno={})", plane_id,
                static_cast<const void*>(layer), result.error().message(), result.error().value());
      return result.error();
    }
  }

  ++test_commits_this_frame_;
  ++diagnostics_.test_commits_issued;
  const auto result = test_req.test(flags);

  if (!result.has_value() && alloc_debug()) {
    alloc_log("[alloc] atomic TEST FAIL: {} (errno={}) — assignment:", result.error().message(),
              result.error().value());
    for (const auto& [plane_id, layer] : assignment) {
      alloc_log("[alloc]   plane={} ← layer={}", plane_id, static_cast<const void*>(layer));
    }
    // EINVAL names no property, so print the ones we asked for. Without this
    // the only way forward is bisecting the set by hand against a driver that
    // accepts the same plane under a smaller one.
    test_req.dump("rejected request");
  }

  // Record in failure cache
  for (const auto& [plane_id, layer] : assignment) {
    failure_cache_.record(plane_id, layer->property_hash(), result.has_value());
  }

  // A single-plane assignment gives clean attribution: the verdict is
  // unambiguously about this (plane, fourcc, modifier). Record it in the modifier
  // probe cache so a lying IN_FORMATS entry costs one probe, not a re-probe every
  // frame. Multi-plane failures can't be pinned on one modifier, so we leave the
  // probe cache untouched for them (the property-hash failure_cache handles those).
  if (assignment.size() == 1) {
    const auto& [plane_id, layer] = *assignment.begin();
    if (const auto fourcc = layer->format(); fourcc.has_value()) {
      probe_cache_.record(crtc_index, plane_id, *fourcc, drm::fmt::Modifier{layer->modifier()},
                          result.has_value());
    }
  }

  if (!result.has_value()) {
    return result.error();
  }
  return {};
}

void Allocator::disable_unused_planes(AtomicRequest& req, const uint32_t crtc_index,
                                      const PlaneAssignment& keep, const bool track_state,
                                      const bool test_only) {
  for (const auto* plane : registry_.for_crtc(crtc_index)) {
    // Cursor planes are owned by a separate code path (the cursor
    // handler) and shouldn't be force-disabled here.
    if (plane->type == DRMPlaneType::CURSOR) {
      continue;
    }
    if (keep.count(plane->id) != 0) {
      continue;
    }
    // External reservations (e.g. the composition canvas plane) — the
    // caller will arm them itself after apply() returns. Skipping the
    // disable here saves two property writes per reserved plane per
    // frame and removes the dependency on kernel last-write-wins
    // semantics for the same atomic request.
    bool externally_reserved = false;
    for (const auto pid : external_reserved_) {
      if (pid == plane->id) {
        externally_reserved = true;
        break;
      }
    }
    if (externally_reserved) {
      continue;
    }
    // Property minimization: when track_state is on, skip emission if
    // the plane is already off (no last_committed_ entry, or the
    // entry's FB_ID is already 0). last_committed_ is the source of
    // truth for what the kernel currently has on this plane;
    // disabling an already-disabled plane is a wasted pair of
    // property writes the kernel still has to walk. The TEST path
    // (track_state=false) always emits because the throwaway request
    // can't rely on the kernel's accumulated state being consulted
    // during atomic_check the same way a real commit does.
    if (track_state && !force_full_writes_) {
      const auto it = last_committed_.find(plane->id);
      const bool already_off = (it == last_committed_.end()) ? !is_foreign_lit(plane->id) : [&] {
        const auto& snap = it->second.properties;
        constexpr auto fb_idx = static_cast<std::size_t>(PropTag::FbId);
        return !snap.set_mask.test(fb_idx) || snap.values.at(fb_idx) == 0U;
      }();
      if (already_off) {
        continue;
      }
    }
    if (const auto fb_prop = prop_store_.property_id(plane->id, "FB_ID"); fb_prop.has_value()) {
      if (req.add_property(plane->id, *fb_prop, 0).has_value() && track_state) {
        ++diagnostics_.properties_written;
        ++diagnostics_.fbs_attached;
      }
    }
    if (const auto crtc_prop = prop_store_.property_id(plane->id, "CRTC_ID");
        crtc_prop.has_value()) {
      if (req.add_property(plane->id, *crtc_prop, 0).has_value() && track_state) {
        ++diagnostics_.properties_written;
      }
    }
    // The plane is now off. Clear its snapshot so the next assignment
    // is treated as a fresh activation (forces a full property write).
    // TEST_ONLY commits don't actually disable the plane in the
    // kernel, so the cache invariant ("snapshot reflects kernel
    // state") would break if we erased on TEST. Same shape as the
    // apply_layer_to_plane_real test_only guard below — kept symmetric.
    if (track_state && !test_only) {
      last_committed_.erase(plane->id);
      drop_foreign_lit(plane->id);
    }
  }
}

void Allocator::read_foreign_lit(const uint32_t crtc_index, const uint32_t crtc_id) {
  foreign_lit_.clear();
  for (const auto* plane : registry_.for_crtc(crtc_index)) {
    if (plane->type == DRMPlaneType::CURSOR) {
      continue;
    }
    // A plane lit on another CRTC is not ours to disable. Unreadable (a
    // synthetic registry) counts as off, as before.
    const std::unique_ptr<drmModePlane, decltype(&drmModeFreePlane)> p(
        drmModeGetPlane(dev_.fd(), plane->id), drmModeFreePlane);
    if (p && p->fb_id != 0 && p->crtc_id == crtc_id) {
      foreign_lit_.push_back(plane->id);
    }
  }
}

bool Allocator::is_foreign_lit(const uint32_t plane_id) const {
  return std::find(foreign_lit_.begin(), foreign_lit_.end(), plane_id) != foreign_lit_.end();
}

void Allocator::drop_foreign_lit(const uint32_t plane_id) noexcept {
  foreign_lit_.erase(std::remove(foreign_lit_.begin(), foreign_lit_.end(), plane_id),
                     foreign_lit_.end());
}

bool Allocator::zpos_is_fixed(const uint32_t plane_id) const {
  for (const auto& p : registry_.all()) {
    if (p.id == plane_id) {
      return detail::zpos_fixed(p);
    }
  }
  return false;
}

bool Allocator::fits_stacking(const PlaneAssignment& assignment, const uint32_t plane_id,
                              const Layer& layer) const {
  const PlaneCapabilities* caps = nullptr;
  for (const auto& p : registry_.all()) {
    if (p.id == plane_id) {
      caps = &p;
    }
  }
  if (caps == nullptr) {
    return true;
  }
  const auto z = layer.property("zpos");
  for (const auto& [other_id, other] : assignment) {
    if (other_id == plane_id || other == nullptr) {
      continue;
    }
    for (const auto& p : registry_.all()) {
      if (p.id == other_id && !detail::stacking_consistent(*caps, z, p, other->property("zpos"))) {
        return false;
      }
    }
  }
  return true;
}

const PlaneCapabilities* Allocator::caps_of(const uint32_t plane_id) const {
  for (const auto& p : registry_.all()) {
    if (p.id == plane_id) {
      return &p;
    }
  }
  return nullptr;
}

bool Allocator::fits_multirect(const PlaneAssignment& assignment, const uint32_t plane_id) const {
  const PlaneCapabilities* caps = caps_of(plane_id);
  return caps == nullptr || detail::multirect_pairing_ok(
                                caps->multirect_parent,
                                [&](uint32_t parent) { return assignment.count(parent) != 0; });
}

bool Allocator::multirect_complete(const PlaneAssignment& assignment) const {
  return std::all_of(assignment.begin(), assignment.end(),
                     [&](const auto& entry) { return fits_multirect(assignment, entry.first); });
}

bool Allocator::stacking_consistent(const PlaneAssignment& assignment) const {
  return std::all_of(assignment.begin(), assignment.end(), [&](const auto& entry) {
    return entry.second == nullptr || fits_stacking(assignment, entry.first, *entry.second);
  });
}

// Rescale a property value into the range the plane actually advertises.
//
// Only alpha needs this today. Layers carry a 16-bit alpha because that is
// what the DRM docs describe, but the range is the driver's to declare and it
// varies: vendor drivers ship 8-bit alpha advertising [0, 255]. Writing 0xFFFF
// to one of those fails the whole atomic commit with EINVAL, and since a
// rejected TEST reads as "this assignment does not fit", it surfaces as a
// silently dropped layer with no mention of alpha.
//
// Rescaled, not clamped -- see rescale_alpha() for why.
//
// Lives here rather than at the layer, which does not know its plane, and is
// shared by both apply paths so they cannot drift.
std::uint64_t Allocator::clamp_to_plane(const uint32_t plane_id, const std::string_view name,
                                        const std::uint64_t value) const {
  if (name != "alpha") {
    return value;
  }
  for (const PlaneCapabilities& caps : registry_.all()) {
    if (caps.id != plane_id) {
      continue;
    }
    return rescale_alpha(value, caps.alpha_max);
  }
  return value;
}

Allocator::ZposStack Allocator::stacked_zpos(const PlaneAssignment& assignment) const {
  std::vector<detail::StackEntry> entries;
  std::vector<uint32_t> ids;
  entries.reserve(assignment.size() + 1);
  ids.reserve(assignment.size() + 1);
  for (const auto& [plane_id, layer] : assignment) {
    entries.push_back({caps_of(plane_id), layer->property("zpos"), std::nullopt});
    ids.push_back(plane_id);
  }
  for (const auto* layer : scratch_current_set_) {
    if (layer->is_pinned() && layer->assigned_plane_.has_value() &&
        assignment.count(*layer->assigned_plane_) == 0) {
      entries.push_back({caps_of(*layer->assigned_plane_), layer->property("zpos"), std::nullopt});
      ids.push_back(*layer->assigned_plane_);
    }
  }
  // The canvas slot, just under the lowest placed layer that must cover the
  // canvas, numbered over the widest settable range on the CRTC (the canvas
  // plane is the scene's choice). Dropped again when the stack has no room.
  PlaneCapabilities slot;
  if (cached_crtc_index_.has_value()) {
    for (const auto* p : registry_.for_crtc(*cached_crtc_index_)) {
      if (p->type == DRMPlaneType::CURSOR || !p->zpos_min.has_value() || !p->zpos_max.has_value() ||
          *p->zpos_min == *p->zpos_max) {
        continue;
      }
      slot.zpos_min = std::min(slot.zpos_min.value_or(*p->zpos_min), *p->zpos_min);
      slot.zpos_max = std::max(slot.zpos_max.value_or(*p->zpos_max), *p->zpos_max);
    }
  }
  bool with_slot = false;
  if (slot.zpos_min.has_value()) {
    const auto bounds = canvas_bounds(assignment);
    with_slot = bounds.above.has_value() && bounds.feasible();
    if (with_slot) {
      entries.push_back({&slot, bounds.above, std::nullopt, /*below_ties=*/true});
    }
  }
  bool fits = detail::stack_zpos(entries);
  if (!fits && with_slot) {
    entries.pop_back();
    fits = detail::stack_zpos(entries);
  }
  ZposStack out;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const std::optional<uint64_t> w = entries[i].written;
    if (!w.has_value()) {
      continue;
    }
    out.written.emplace_back(ids[i], *w);
    if (fits && w != entries[i].requested) {
      out.renumbered.emplace_back(ids[i], *w);
    }
  }
  if (fits && entries.size() > ids.size()) {
    out.canvas = entries.back().written;
  }
  return out;
}

void Allocator::record_stack(const ZposStack& stack, const bool any_composited) {
  zpos_stack_ = stack.written;
  canvas_zpos_ = any_composited ? stack.canvas : std::nullopt;
}

Allocator::CanvasBounds Allocator::canvas_bounds(const PlaneAssignment& assignment) const {
  scratch_bounds_placed_.clear();
  scratch_bounds_composited_.clear();
  for (const auto& entry : assignment) {
    scratch_bounds_placed_.push_back(entry.second);
  }
  for (const auto* layer : scratch_current_set_) {
    if (layer->is_composition_layer() || layer->is_externally_bound()) {
      continue;
    }
    if (layer->is_pinned()) {
      if (layer->assigned_plane_.has_value() && assignment.count(*layer->assigned_plane_) == 0) {
        scratch_bounds_placed_.push_back(layer);
      }
    } else if (std::none_of(assignment.begin(), assignment.end(),
                            [layer](const auto& e) { return e.second == layer; })) {
      scratch_bounds_composited_.push_back(layer);
    }
  }
  CanvasBounds bounds;
  for (const auto* placed : scratch_bounds_placed_) {
    const auto zp = placed->property("zpos");
    if (!zp.has_value()) {
      continue;
    }
    for (const auto* composited : scratch_bounds_composited_) {
      const auto zc = composited->property("zpos");
      if (!zc.has_value() || *zc == *zp || !layers_intersect(*placed, *composited)) {
        continue;
      }
      if (*zc < *zp) {
        bounds.above = std::min(bounds.above.value_or(*zp), *zp);
      } else {
        bounds.below = std::max(bounds.below.value_or(*zp), *zp);
      }
    }
  }
  return bounds;
}

std::optional<PlaneAssignment> Allocator::place_around_run(
    const Output& output, const std::size_t placed,
    const std::vector<const PlaneCapabilities*>& planes, const uint32_t flags,
    const uint32_t crtc_index) {
  // Candidates in zpos order (output is sorted); forced layers must be in the run.
  std::vector<Layer*> layers;
  for (auto* l : output.layers()) {
    if (!l->is_composition_layer() && !l->is_externally_bound() && !l->is_pinned()) {
      layers.push_back(l);
    }
  }
  const std::size_t n = layers.size();
  std::size_t first_forced = n;
  std::size_t last_forced = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (layers[i]->force_composited_ || layers[i]->is_transient_composited()) {
      first_forced = std::min(first_forced, i);
      last_forced = i;
    }
  }
  // Runs of the composited count and up, the cheapest in keep_priority per size.
  for (std::size_t w = std::max<std::size_t>(n - std::min(placed, n), 1); w < n; ++w) {
    if (test_commits_this_frame_ >= max_test_commits_) {
      break;
    }
    std::optional<std::size_t> best;
    long long best_cost = 0;
    for (std::size_t a = 0; a + w <= n; ++a) {
      if (first_forced < n && (a > first_forced || a + w <= last_forced)) {
        continue;
      }
      long long cost = 0;
      for (std::size_t i = a; i < a + w; ++i) {
        cost += keep_priority(*layers[i]);
      }
      // On a tie the higher run: a run at the top keeps the canvas on top.
      if (!best.has_value() || cost <= best_cost) {
        best = a;
        best_cost = cost;
      }
    }
    if (!best.has_value()) {
      continue;
    }
    std::vector<Layer*> outside;
    outside.reserve(n - w);
    for (std::size_t i = 0; i < n; ++i) {
      if (i < *best || i >= *best + w) {
        outside.push_back(layers[i]);
      }
    }
    auto assignment = place_group(outside, planes, flags, crtc_index);
    alloc_log("[alloc] canvas run [{}, {}): {} of {} placed around it", *best, *best + w,
              assignment.size(), outside.size());
    if (!assignment.empty() && canvas_bounds(assignment).feasible()) {
      return assignment;
    }
  }
  return std::nullopt;
}

bool Allocator::resolve_canvas_bounds(PlaneAssignment& assignment) const {
  for (auto bounds = canvas_bounds(assignment); !bounds.feasible();
       bounds = canvas_bounds(assignment)) {
    if (!bounds.above.has_value() || !bounds.below.has_value()) {
      return true;  // unreachable: infeasible needs both
    }
    const uint64_t above = *bounds.above;
    const uint64_t below = *bounds.below;
    // Caught: a placed layer overlapping a composited one, from the lowest
    // that must cover the canvas to the highest that must stay under it.
    // canvas_bounds() left the composited layers in its scratch.
    std::optional<uint32_t> victim;
    int victim_keep = 0;
    for (const auto& entry : assignment) {
      const Layer* layer = entry.second;  // a structured binding can't be captured in C++17
      const auto z = layer->property("zpos");
      if (!z.has_value() || *z < above || *z > below ||
          std::none_of(scratch_bounds_composited_.begin(), scratch_bounds_composited_.end(),
                       [&](const Layer* c) {
                         const auto zc = c->property("zpos");
                         return zc.has_value() && *zc != *z && layers_intersect(*layer, *c);
                       })) {
        continue;
      }
      const int keep = keep_priority(*layer);
      if (!victim.has_value() || keep < victim_keep) {
        victim = entry.first;
        victim_keep = keep;
      }
    }
    if (!victim.has_value()) {
      return false;
    }
    alloc_log("[alloc] canvas bounds [{}, {}] infeasible: compositing the layer on plane {}", below,
              above, *victim);
    assignment.erase(*victim);
    for (auto it = assignment.begin(); it != assignment.end();) {
      const auto* c = caps_of(it->first);
      it = (c != nullptr && c->multirect_parent == *victim) ? assignment.erase(it) : std::next(it);
    }
  }
  return true;
}

drm::expected<void, std::error_code> Allocator::apply_layer_to_plane(
    const Layer& layer, const uint32_t plane_id, AtomicRequest& req,
    const std::optional<uint64_t> zpos) const {
  for (auto [name, value] : layer.properties()) {
    if (name == "zpos" && zpos.has_value()) {
      value = *zpos;
    }
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
    // Fixed-slot zpos (min == max) is not settable even without the
    // IMMUTABLE flag — writing it EINVALs on e.g. vc4's PRIMARY.
    if (name == "zpos" && zpos_is_fixed(plane_id)) {
      continue;
    }
    // Clamp alpha into the plane's advertised range. Layers carry a 16-bit
    // alpha because that is what the DRM docs describe, but the range is the
    // driver's to declare and vendor drivers ship 8-bit alpha advertising
    // [0, 255]. Writing 0xFFFF there fails the whole commit with EINVAL, and
    // because a rejected TEST just means "this assignment does not fit", it
    // surfaces as a silently dropped layer with no mention of alpha.
    if (auto result = req.add_property(plane_id, *prop_id, clamp_to_plane(plane_id, name, value));
        !result.has_value()) {
      return result;
    }
  }
  return {};
}

drm::expected<void, std::error_code> Allocator::apply_layer_to_plane_real(
    const Layer& layer, const uint32_t plane_id, AtomicRequest& req, const bool test_only,
    const std::optional<uint64_t> zpos) {
  // Decide whether this is a full re-emit (every property written
  // unconditionally) or a per-property diff against last frame's
  // snapshot. Full-write triggers:
  //   - force_full_writes_ is on (driver-quirk opt-out).
  //   - last_committed_ has no entry for this plane (first activation
  //     since the allocator was constructed, or since the plane was
  //     last detached by disable_unused_planes).
  //   - the entry's stored layer pointer differs from the new one,
  //     which means the plane is being reassigned to a different
  //     scene-side Layer; without a full write the new layer would
  //     inherit any property the old layer had set but the new one
  //     hasn't (e.g. rotation, alpha).
  const auto it = last_committed_.find(plane_id);
  const bool full_write =
      force_full_writes_ || it == last_committed_.end() || it->second.layer != &layer;

  for (std::size_t i = 0; i < k_num_props; ++i) {
    if (!layer.set_mask_.test(i)) {
      continue;
    }
    const auto tag = static_cast<PropTag>(i);
    const auto name = prop_name(tag);
    const auto value = (tag == PropTag::Zpos && zpos.has_value()) ? *zpos : layer.values_.at(i);
    auto prop_id = prop_store_.property_id(plane_id, name);
    if (!prop_id.has_value()) {
      continue;
    }
    if (prop_store_.is_immutable(plane_id, name).value_or(false)) {
      continue;
    }
    // Fixed-slot zpos (min == max) is not settable even without the
    // IMMUTABLE flag — writing it EINVALs on e.g. vc4's PRIMARY.
    if (tag == PropTag::Zpos && zpos_is_fixed(plane_id)) {
      continue;
    }
    // Externally-bound layers (EGL stream sources) have their FB_ID
    // set up by the producer-side extension stack; suppress the
    // scene-side write defensively even if a caller has somehow
    // stuffed FB_ID into the layer's property bag.
    if (layer.is_externally_bound() && tag == PropTag::FbId) {
      continue;
    }
    bool need_write = full_write;
    if (!need_write) {
      const auto& snap = it->second.properties;
      need_write = !snap.set_mask.test(i) || snap.values.at(i) != value;
    }
    // FB_ID always emits, regardless of the snapshot diff. KMS treats
    // FB_ID re-attachment as the "this is a new frame" signal — the
    // kernel uses it to schedule the page-flip event a non-blocking
    // commit relies on. Suppressing the write when the value is
    // unchanged (single-buffer source like DumbBufferSource where
    // pixel bytes mutate in place between commits) leaves the
    // AtomicRequest empty for an otherwise-clean scene; the kernel
    // accepts the empty commit but never queues PAGE_FLIP_EVENT, and
    // the caller's flip_pending wedges true. The other plane
    // properties (CRTC_*, SRC_*, alpha, zpos, rotation) still benefit
    // from the diff, which is the bulk of the per-frame property
    // traffic.
    // IN_FENCE_FD is a per-frame one-shot the kernel consumes on each commit:
    // an fd that the diff happens to see as unchanged (recycled value) must
    // still be re-armed, or the plane scans out before its buffer is ready.
    if (prop_class(tag) == PropClass::Content) {
      need_write = true;
    }
    if (!need_write) {
      continue;
    }
    if (auto result =
            req.add_property(plane_id, *prop_id, clamp_to_plane(plane_id, prop_name(tag), value));
        !result.has_value()) {
      return result;
    }
    ++diagnostics_.properties_written;
    if (tag == PropTag::FbId) {
      ++diagnostics_.fbs_attached;
    }
  }
  // Update the snapshot to reflect what's now committed for this
  // plane. The snapshot is a trivially-copyable fixed-size POD now —
  // memcpy, no allocations. Storing a copy (rather than aliasing the
  // layer's live state) preserves the invariant that a future
  // relayout that rewrites layer properties can't change the
  // baseline we diff against on the next commit.
  //
  // TEST_ONLY commits don't reach the kernel's committed state, so
  // they must NOT update the snapshot — otherwise a subsequent
  // commit (or another TEST_ONLY) sees a poisoned cache and the
  // diff suppresses properties the kernel still needs (most visibly:
  // CRTC_ID + dest rect + src rect under MODESET, where the kernel
  // rejects the partial commit with EINVAL).
  if (!test_only) {
    // Record the zpos written (the stacked value), so the next frame diffs
    // against what the kernel has.
    auto snap = layer.snapshot();
    if (zpos.has_value() && snap.set_mask.test(static_cast<std::size_t>(PropTag::Zpos))) {
      snap.values.at(static_cast<std::size_t>(PropTag::Zpos)) = *zpos;
    }
    last_committed_[plane_id] = LastCommitted{&layer, snap, layer.property_hash()};
    drop_foreign_lit(plane_id);
  }
  return {};
}

// ── Backtracking search ───────────────────────────────────────

bool Allocator::backtrack(std::vector<Layer*>& layers,
                          const std::vector<const PlaneCapabilities*>& planes,
                          PlaneAssignment& assignment, const std::size_t depth,
                          const std::size_t best_so_far, AtomicRequest& req, const uint32_t flags,
                          uint32_t crtc_index) {
  if (depth >= layers.size()) {
    return true;
  }
  if (test_commits_this_frame_ >= max_test_commits_) {
    return false;
  }

  Layer* layer = layers.at(depth);

  // Skip force-composited layers (user-set or scene-driven transient).
  if (layer->force_composited_ || layer->is_transient_composited()) {
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
    if (probe_rejected(crtc_index, plane->id, *layer)) {
      continue;
    }
    if (!fits_stacking(assignment, plane->id, *layer) || !fits_multirect(assignment, plane->id)) {
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
