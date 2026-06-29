// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// external_dma_buf_ring.hpp — LayerBufferSource over an N-slot ring of
// caller-owned, externally-allocated DMA-BUFs, for a *rotating* producer
// that hands in a fresh slot (and optionally a render-done fence) per frame.
//
// Where ExternalDmaBufSource (scene/external_dma_buf_source.hpp) is single-use
// (one cached fb_id, on_release fires once), this type registers one fb_id per
// producer slot at create() and rotates among them under submit(). It is the
// external-producer analogue of present::DumbRingSource — same SceneSubmitsFbId
// binding, same `opaque == slot index` convention — but the buffers come from
// outside via PRIME import rather than being allocated by the ring.
//
// Motivating consumers (compositor-shared fault domain, not a standalone loop):
//   * water — WebGPU/Vulkan producer. Renders a tiled/AFBC slot per frame on its
//     own thread and exports a render-done sync_file; passes it to submit() as
//     the acquire fence. Wants the per-slot release fence to re-render slot K
//     with no CPU wait.
//   * CEF — CPU-side producer. submit(slot) with no fence; CPU-ready buffers.
//
// Threading: submit() runs on the producer thread while the scene
// calls acquire()/release() on its commit thread. The pending-slot/fence handoff
// is mutex-guarded so submit() is safe concurrent with acquire()/release().
// scanning-slot bookkeeping is touched only on the commit thread.
//
// Idle-hold: when no fresh submit() arrives, acquire() re-hands the
// currently-scanning slot (holding the last good frame) instead of returning
// EAGAIN — a dropped layer would blank the plane. Only a genuinely *replaced*
// slot signals release.
//
// FD stability: each slot's plane fds are dup'd at create() and
// closed only at destroy; the recycled fds are stable across frames.
//
// The OUT_FENCE-carrying release form requires the scene to hand
// back the replacing commit's OUT_FENCE; until that scene plumbing lands,
// release() fires the callback/event-edge form on_release(slot, nullopt), which
// is exactly the CEF path and the OUT_FENCE-less (vkms / VOP2) fallback path.

#pragma once

#include "buffer_source.hpp"
#include "external_dma_buf_source.hpp"  // ExternalPlaneInfo

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// One slot of the ring: a complete externally-allocated buffer (1..4 dma-buf
/// planes) plus the modifier the producer allocated it with. `planes` is read at
/// create() (its fds are dup'd) and need not outlive the call.
struct ExternalSlotDesc {
  std::uint64_t modifier{0};
  drm::span<const ExternalPlaneInfo> planes;
};

class ExternalDmaBufRing : public LayerBufferSource {
 public:
  /// Slot K left scanout. `release_fence`, when present, signals GPU-side once K
  /// is safe to render into (the OUT_FENCE of the commit that replaced K);
  /// nullopt means the callback edge itself is the "slot free" signal (CEF, or
  /// any CRTC without OUT_FENCE_PTR). Fires on the scene's commit thread.
  using OnSlotRelease =
      std::function<void(std::size_t slot, std::optional<drm::sync::SyncFence> release_fence)>;

  // Default member initializers keep `Options opts;` safe to default-construct
  // (the raw `validate_against` pointer would otherwise be indeterminate). The
  // create() overload split below avoids forming an in-class default argument
  // from this DMI-bearing aggregate, which the language forbids until the
  // enclosing class is complete.
  struct Options {
    OnSlotRelease on_release;

    /// Fault isolation. When set, `acquire()` **CPU-pre-waits** the
    /// producer fence up to this deadline and NEVER hands it to the kernel via
    /// IN_FENCE_FD — a never-signaling in-fence the kernel already holds wedges
    /// the whole-CRTC pipeline (the blast radius this guards); the two are
    /// mutually exclusive. On a miss the ring **holds the last good slot**
    /// (frozen-but-alive, not blank), reusing the idle-hold path; the producer
    /// keeps submitting and whichever frame next signals in time advances
    /// (auto-recovery). nullopt = legacy pass-through (the scene wires IN_FENCE_FD
    /// or CPU-waits). Enforced entirely in the ring — no LayerScene change.
    std::optional<std::chrono::nanoseconds> fence_deadline;

    /// Validate-not-negotiate. When non-null, each slot's modifier is checked
    /// against this plane's IN_FORMATS before drmModeAddFB2WithModifiers;
    /// create() returns errc::not_supported on a miss so the caller can
    /// re-negotiate (LayerScene::candidate_modifiers) or mark the layer
    /// force_composited. Typically ScanoutTarget::primary_formats.
    const fmt::FormatTable* validate_against{nullptr};
  };

  /// Register one fb_id per slot on `dev`. All slots share width/height/fourcc;
  /// each carries its own planes + modifier. See file comment for the contract.
  [[nodiscard]] static drm::expected<std::unique_ptr<ExternalDmaBufRing>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format,
      drm::span<const ExternalSlotDesc> slots, Options options);

  /// Overload with default Options (empty callback, no deadline, no validation).
  [[nodiscard]] static drm::expected<std::unique_ptr<ExternalDmaBufRing>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format,
      drm::span<const ExternalSlotDesc> slots);

  ExternalDmaBufRing(const ExternalDmaBufRing&) = delete;
  ExternalDmaBufRing& operator=(const ExternalDmaBufRing&) = delete;
  ExternalDmaBufRing(ExternalDmaBufRing&&) = delete;
  ExternalDmaBufRing& operator=(ExternalDmaBufRing&&) = delete;
  ~ExternalDmaBufRing() override;

  /// Producer hands in the slot to scan out next and (optionally) its render-done
  /// fence. Thread-safe vs acquire()/release(). `slot` out of range is ignored
  /// (logged under DRM_EXT_DMABUF_DEBUG). The next acquire() returns this slot.
  ///
  /// `damage` is the slot's per-frame dirty-region list (buffer pixels,
  /// top-left origin); the next acquire() emits it as `AcquiredBuffer::damage`
  /// so the scene can drive FB_DAMAGE_CLIPS. Empty = whole-frame. Two rules the
  /// caller must know:
  ///   * **Replace, not union** — each submit() overwrites the pending damage;
  ///     the ring can't tell whether two submits are sequential, so accumulating
  ///     damage across producer-side cross-drops is the *producer's* job (the
  ///     harness FrameMailbox does it). The exception is the ring's own
  ///     fence-deadline drop, which it accumulates itself (see fence_deadline).
  ///   * **Over-cap → whole-frame** — more than k_max_damage rects degrades to
  ///     empty (whole-frame), never truncates (a truncated list under-reports the
  ///     dirty area and corrupts the scanout).
  void submit(std::size_t slot, std::optional<drm::sync::SyncFence> acquire = std::nullopt,
              drm::span<const DamageRect> damage = {}) noexcept;

  /// Fault isolation: the per-layer fence deadline the scene must enforce, if any.
  [[nodiscard]] std::optional<std::chrono::nanoseconds> fence_deadline() const noexcept {
    return fence_deadline_;
  }

  /// Number of registered slots.
  [[nodiscard]] std::size_t slot_count() const noexcept { return slots_.size(); }

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  void release_with_fence(AcquiredBuffer acquired,
                          std::optional<drm::sync::SyncFence> release_fence) noexcept override;
  /// True once an on_release callback is registered — the scene then delivers
  /// the release fence via release_with_fence.
  [[nodiscard]] bool wants_release_fence() const noexcept override {
    return static_cast<bool>(on_release_);
  }
  /// True iff a submit awaits acquisition — i.e. the producer has a fresh frame
  /// the next commit will scan out (vs an idle hold-last-frame). Drives the
  /// scene's all-idle Skip. Thread-safe vs submit()/acquire().
  [[nodiscard]] bool has_fresh_content() const noexcept override { return has_fresh_frame(); }
  [[nodiscard]] bool has_fresh_frame() const noexcept {
    const std::scoped_lock lock(mu_);
    return pending_slot_.has_value();
  }
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return format_; }
  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 private:
  ExternalDmaBufRing() = default;

  static constexpr std::size_t k_max_planes = 4;

  // Fixed damage store so submit() stays alloc-free / noexcept; the vector copy
  // into AcquiredBuffer::damage happens in acquire() (already non-noexcept).
  // Above this count, submit() degrades to whole-frame (empty) rather than
  // truncating — under-reporting the dirty area would corrupt the scanout.
  static constexpr std::size_t k_max_damage = 16;

  struct PlaneRecord {
    int duped_fd{-1};
    std::uint32_t gem_handle{0};
    std::uint32_t offset{0};
    std::uint32_t pitch{0};
  };

  struct SlotRecord {
    std::array<PlaneRecord, k_max_planes> planes{};
    std::size_t plane_count{0};
    std::uint32_t fb_id{0};
    std::uint64_t modifier{0};
  };

  /// Build one slot's GEM handles + FB on `fd`. Used by create() and resume.
  /// const: mutates only the passed-in `slot`, never *this.
  [[nodiscard]] drm::expected<void, std::error_code> import_slot(int fd, SlotRecord& slot) const;
  /// Drop FB + GEM handles for every slot (keeps duped fds). Idempotent.
  void teardown_kernel_state() noexcept;
  void close_duped_fds() noexcept;
  void fire_release(std::size_t slot, std::optional<drm::sync::SyncFence> release_fence) noexcept;

  /// Accumulate the damage of a frame this ring just dropped on a fence-deadline
  /// miss into carried_damage_, so the next presented frame repaints those never-
  /// presented regions. `count == 0` (the dropped frame was whole-frame) collapses
  /// the carry to whole-frame; so does an accumulation past k_max_damage.
  /// Commit-thread only; no lock.
  void accumulate_carried_damage(const std::array<DamageRect, k_max_damage>& buf,
                                 std::size_t count) noexcept;
  /// Fill `out` for a fresh frame: the union of carried_damage_ (dropped frames)
  /// and this frame's own damage (`buf`/`count`). Empty `out` == whole-frame.
  /// Clears the carry. Commit-thread only.
  void take_damage_with_carry(std::vector<DamageRect>& out,
                              const std::array<DamageRect, k_max_damage>& buf, std::size_t count);

  int fd_{-1};
  SourceFormat format_{};
  std::vector<SlotRecord> slots_;
  OnSlotRelease on_release_;
  std::optional<std::chrono::nanoseconds> fence_deadline_;

  // Producer→commit-thread handoff, guarded by mu_. Mutable so the const
  // has_fresh_frame() query can lock it.
  mutable std::mutex mu_;
  std::optional<std::size_t> pending_slot_;
  std::optional<drm::sync::SyncFence> pending_fence_;
  // Per-frame damage for the pending slot, replaced on each submit(). count == 0
  // means whole-frame — either the caller passed no rects, or an over-cap submit
  // degraded to whole-frame. Drained atomically with pending_slot_ in acquire().
  std::array<DamageRect, k_max_damage> pending_damage_{};
  std::size_t pending_damage_count_{0};

  // Commit-thread only. Each fresh advance gets a new monotonic token (pointer-
  // width so it round-trips through AcquiredBuffer::opaque without truncation on
  // ILP32); idle re-holds reuse the live token. `outstanding_` maps each live
  // token to its slot; release fires once per token by erasing its entry, keyed
  // by token not slot index so the scene's triple-deferred release of an aliased
  // slot (slot 0 retiring while slot 0 is live again two frames later) resolves
  // correctly. Bounded by the in-flight depth, so a linear scan is cheap.
  struct Outstanding {
    std::uintptr_t token{0};
    std::size_t slot{0};
  };
  std::optional<std::size_t> scanning_slot_;
  std::uintptr_t next_token_{0};
  std::uintptr_t scanning_token_{0};
  std::vector<Outstanding> outstanding_;

  // Damage carried across this ring's own fence-deadline drops (commit-thread
  // only, like scanning_slot_). A dropped frame's dirty regions were never
  // presented, so they must be unioned into the next presented frame or it
  // under-reports relative to the dropped one. `carried_damage_whole_` collapses
  // the list to whole-frame when a dropped frame was itself whole-frame or the
  // accumulation overflows k_max_damage. Empty/false unless fence_deadline_ is
  // set and a drop occurred, so the no-deadline (CEF) path never touches it.
  std::array<DamageRect, k_max_damage> carried_damage_{};
  std::size_t carried_damage_count_{0};
  bool carried_damage_whole_{false};
};

}  // namespace drm::scene
