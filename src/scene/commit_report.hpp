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
// (property minimization), 2.3 (composition fallback), and
// 2.4 (rebind) each fill in more.

#pragma once

#include "layer_handle.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace drm::scene {

/// How a layer reached scanout in a given commit. Pairs with the
/// `plane_id` field of `LayerPlacementEntry` to describe the layer's
/// outcome.
enum class LayerPlacement : std::uint8_t {
  /// Layer reached scanout on its own hardware plane. `plane_id` is
  /// the plane the allocator picked.
  AssignedToPlane,
  /// Layer was composited into the canvas plane. `plane_id` is the
  /// canvas plane.
  Composited,
  /// Layer was dropped this frame (no hardware plane available, no
  /// CPU mapping for composition, or canvas allocation failed).
  /// `plane_id` is 0.
  Unassigned,
};

/// One row of `CommitReport::placements`. Identifies a scene layer and
/// its outcome for the commit that produced the report.
struct LayerPlacementEntry {
  LayerHandle handle;
  LayerPlacement placement{LayerPlacement::Unassigned};
  /// The plane id the layer's content reached scanout on, or 0 when
  /// `placement == Unassigned`. For composited layers this is the
  /// canvas plane id, not a per-layer plane.
  std::uint32_t plane_id{0};
  /// The assigned plane's supported rotation/reflect angles (a mask of
  /// DRM_MODE_ROTATE_* | DRM_MODE_REFLECT_* bits) for `AssignedToPlane`
  /// layers; 0 for `Composited`/`Unassigned` or a plane with no rotation
  /// property. Lets a producer decide whether the layer's requested angle
  /// can land on its bound plane or needs pre-rotation.
  std::uint64_t plane_rotation_bits{0};
};

/// Diagnostic snapshot of a single `LayerScene::commit()` or `test()`
/// call. Every field is a non-negative count; consumers use these for
/// unit-test assertions, benchmark instrumentation, and runtime
/// telemetry. Fields the current phase doesn't populate stay at 0.
struct CommitReport {
  /// Total layers in the scene at commit time.
  /// Invariant:
  ///   `layers_total == layers_assigned + layers_composited
  ///                  + layers_unassigned + layers_skipped_no_frame`.
  std::size_t layers_total{0};
  /// Layers the allocator placed directly on a hardware plane this
  /// commit (not via the composition canvas).
  std::size_t layers_assigned{0};
  /// Layers rescued by software composition into a `CompositeCanvas`
  /// bucket. They do reach hardware, just via the canvas plane rather
  /// than their own plane. 0 when the allocator placed everything.
  std::size_t layers_composited{0};
  /// Layers neither hardware-placed nor composited — dropped this
  /// frame. Set when the canvas pool was full, no free plane could
  /// host the canvas, or the layer's source had no CPU mapping for
  /// the compositor to read from.
  std::size_t layers_unassigned{0};
  /// Layers whose source returned `errc::resource_unavailable_try_again`
  /// (EAGAIN) from `acquire()` and were skipped for this commit. EAGAIN
  /// is flow control, not a failure: a live source has no frame to
  /// contribute this vblank (typically pre-preroll, or a producer that
  /// fell behind without a cached frame to re-issue). The next commit
  /// re-calls `acquire()`. Consumers can graph this as a "frame stall"
  /// signal per layer.
  std::size_t layers_skipped_no_frame{0};
  /// Number of composition buckets emitted this frame. 0 or 1 in v1
  /// of (single full-screen canvas); multi-canvas pooling
  /// can raise this.
  std::size_t composition_buckets{0};

  /// Layers that requested `LayerDesc::pin_to_plane` but whose pin could
  /// not be honored this commit — the target plane isn't on this CRTC,
  /// doesn't support the layer's format, or was already claimed. The
  /// layer is NOT dropped: it falls back to normal allocation and is
  /// still counted in one of the placement tallies above. Nonzero is a
  /// loud signal that a caller's deterministic-plane assumption was
  /// violated (as opposed to a silent drop). Does not affect the
  /// `layers_total` invariant — it is orthogonal to placement outcome.
  std::size_t pins_failed{0};

  /// Total properties enqueued on the AtomicRequest this commit —
  /// includes plane state, CRTC state, and connector state. Useful as
  /// a regression signal for property-minimization work:
  /// the count should drop meaningfully on unchanged-layer commits.
  std::size_t properties_written{0};
  /// Subset of `properties_written` that are FB_ID attachments. One
  /// per plane per frame when a layer's source produced a new fb_id;
  /// zero on the cheap path where an unchanged layer keeps its prior
  /// FB binding.
  std::size_t fbs_attached{0};
  /// Subset of `properties_written`: layers that got an FB_DAMAGE_CLIPS blob
  /// this commit — one per natively-placed layer whose source reported a
  /// non-empty, in-bounds damage region within the rect budget. Zero on a
  /// full-frame commit (no damage, over budget, degenerate clips, or a plane /
  /// driver without the property). The census signal for the damaged-commit
  /// path: it tells a scanout-bandwidth-sensitive consumer whether partial
  /// updates are actually reaching the kernel.
  std::size_t damage_clips_armed{0};

  /// Layers whose acquire fence was handed to the kernel as the plane's
  /// IN_FENCE_FD this commit — i.e. the real explicit-sync path: the display
  /// engine waits on the producer's fence before sampling the buffer, no CPU
  /// stall. One per natively-placed layer that carried an `acquire_fence` and
  /// whose plane exposes a mutable IN_FENCE_FD property. The honest signal that
  /// explicit sync is live on this driver — distinguishes it from the
  /// `in_fence_cpu_waits` fallback below (a source may always produce a fence,
  /// yet the plane not accept it).
  std::size_t in_fences_armed{0};
  /// Layers that carried an acquire fence but whose plane had no usable
  /// IN_FENCE_FD property, so the scene blocked on a CPU wait for the fence
  /// before committing (real commits only; a TEST pass never waits). Nonzero
  /// means the driver/plane can't do KMS-side explicit sync for this layer and
  /// the present path paid a CPU stall instead. `in_fences_armed == 0 &&
  /// in_fence_cpu_waits == 0` means no source produced an acquire fence at all.
  std::size_t in_fence_cpu_waits{0};

  /// Internal test commits issued by the allocator while searching
  /// for a valid plane assignment. Cold-start commits can spend the
  /// whole budget (default 16); warm-start commits are 1, and the
  /// FB-only fast path (see `fb_delta_fast_path`) is 0.
  std::size_t test_commits_issued{0};

  /// true when this commit took the FB-only fast path: only content
  /// (FB_ID / damage / fence) changed on already-placed layers, so the
  /// allocator reused the cached plane assignment and skipped the
  /// TEST_ONLY re-validation entirely (`test_commits_issued == 0`). The
  /// steady-state signal a single-layer consumer asserts to confirm it is
  /// paying one atomic commit per frame, not two.
  bool fb_delta_fast_path{false};

  /// true when the scene's auto-derive built an
  /// `HdrSourceMetadata` from a layer's `source_eotf` but the
  /// connector can't signal HDR (no `HDR_OUTPUT_METADATA` exposed,
  /// or `max_bpc` capped below 10) and the scene degraded to SDR
  /// for this commit. Without 10-bit depth at the sink, HDR PQ is
  /// 8-bit-tone-mapped-with-a-PQ-flag, which the sink accepts but
  /// doesn't display as HDR — the design's "no silent banding"
  /// invariant. Always false for a manual `set_output_metadata`
  /// override (the manual path bypasses the auto-derive's
  /// constraint check).
  bool hdr_downgraded_no_max_bpc{false};

  /// true when a FrameEconomy-driven presenter suppressed this frame's
  /// commit because nothing changed since the last committed frame (the
  /// idle-Skip: no atomic commit, no page flip, no scanout reprogram — a
  /// power win on every panel, and it lets a PSR-capable panel stay in
  /// self-refresh). When set, every count above is 0 and `placements` is
  /// empty: no commit was issued. Only `ScanoutBackend::present_if_changed()`
  /// (and other economy-aware presenters) set this; `LayerScene::commit()`
  /// itself always commits and never sets it.
  bool skipped_idle{false};

  /// One entry per layer the scene attempted to place this commit, in
  /// the scene's layer-iteration order. Empty when the scene had no
  /// layers or the commit failed before placement ran. Same ordering
  /// across `test()` and `commit()` for an unchanged scene, so callers
  /// can pair a `--probe` `test()` report against a real `commit()`
  /// report verbatim.
  std::vector<LayerPlacementEntry> placements;

  /// This layer's placement outcome this commit, or nullopt when it isn't in
  /// `placements` (removed, EAGAIN-skipped, or the commit produced none). Saves
  /// every consumer from re-scanning `placements` by hand.
  [[nodiscard]] std::optional<LayerPlacementEntry> placement_of(LayerHandle handle) const {
    for (const LayerPlacementEntry& entry : placements) {
      if (entry.handle == handle) {
        return entry;
      }
    }
    return std::nullopt;
  }

  /// True iff the layer was demoted to software composition (the composition
  /// fallback) rather than reaching its own hardware plane. A zero-copy producer
  /// (e.g. an ExternalDmaBufRing carrying a tiled/AFBC buffer) watches this to
  /// notice it has silently dropped to a LINEAR canvas copy and react —
  /// re-negotiate a scannable modifier, drop resolution, etc.
  [[nodiscard]] bool was_composited(LayerHandle handle) const {
    const std::optional<LayerPlacementEntry> entry = placement_of(handle);
    return entry.has_value() && entry->placement == LayerPlacement::Composited;
  }
};

}  // namespace drm::scene
