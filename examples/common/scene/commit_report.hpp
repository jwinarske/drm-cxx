// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// commit_report.hpp — diagnostic snapshot of one scene commit.
//
// Returned by LayerScene::commit() and LayerScene::test(). Consumers
// use it for: unit-test assertions (was FB_ID written this frame?
// were any test commits issued?), benchmark instrumentation (how many
// properties did property-minimization elide?), and telemetry.
//
// Every counter is a non-negative integer — no "not applicable"
// sentinels. Fields the current phase doesn't populate yet stay at 0;
// Phase 2.2 (property minimization), 2.3 (composition fallback), and
// 2.4 (rebind) each fill in more.

#pragma once

#include <cstddef>

namespace drm::scene {

struct CommitReport {
  /// Total layers in the scene at commit time.
  std::size_t layers_total{0};
  /// Layers the allocator placed on a hardware plane this commit.
  std::size_t layers_assigned{0};
  /// layers_total - layers_assigned. In Phase 2.1 these are simply
  /// dropped (logged via drm::log_warn); Phase 2.3 composites them
  /// into CompositeCanvas buckets.
  std::size_t layers_unassigned{0};
  /// Number of composition buckets emitted this frame. 0 until Phase
  /// 2.3 lands the compositor.
  std::size_t composition_buckets{0};

  /// Total properties enqueued on the AtomicRequest this commit —
  /// includes plane state, CRTC state, and connector state. Useful as
  /// a regression signal for property-minimization work (Phase 2.2):
  /// the count should drop meaningfully on unchanged-layer commits.
  std::size_t properties_written{0};
  /// Subset of `properties_written` that are FB_ID attachments. One
  /// per plane per frame when a layer's source produced a new fb_id;
  /// zero on the cheap path where an unchanged layer keeps its prior
  /// FB binding.
  std::size_t fbs_attached{0};

  /// Internal test commits issued by the allocator while searching
  /// for a valid plane assignment. Cold-start commits can spend the
  /// whole budget (default 16); warm-start commits should normally
  /// be 0.
  std::size_t test_commits_issued{0};
};

}  // namespace drm::scene
