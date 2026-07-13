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
// Two release-delivery forms, both live. When the replacing commit carries an
// OUT_FENCE, the scene hands it back through release_with_fence() and the ring
// forwards it as on_release(slot, fence) so a GPU producer can re-render slot K
// with no CPU wait (the water path). When there is no OUT_FENCE (vkms / VOP2)
// or the producer is CPU-side (CEF), release() delivers the callback/event-edge
// form on_release(slot, nullopt) instead.

#pragma once

#include "buffer_source.hpp"  // ExternalPlaneInfo
#include "detail/dmabuf_slot.hpp"
#include "detail/external_ring_core.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
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
    return presenter_.fence_deadline();
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
  [[nodiscard]] bool has_fresh_frame() const noexcept { return presenter_.has_fresh_frame(); }
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return format_; }
  // Export the currently-scanning slot's DMA-BUF planes for the GPU
  // compositor's EGLImage import path (borrowed fds — see DmaBufDesc).
  // Commit-thread only, like scanning_slot_; nullopt before the first
  // acquire() returns function_not_supported.
  [[nodiscard]] drm::expected<DmaBufDesc, std::error_code> export_dma_buf() override {
    const auto scanning = presenter_.scanning_key();
    if (!scanning.has_value() || *scanning >= slots_.size()) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::function_not_supported));
    }
    const SlotRecord& s = slots_.at(*scanning);
    if (s.plane_count == 0) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::function_not_supported));
    }
    DmaBufDesc d;
    d.n_planes = static_cast<std::uint32_t>(s.plane_count);
    d.drm_fourcc = format_.drm_fourcc;
    d.modifier = s.modifier;
    d.width = format_.width;
    d.height = format_.height;
    for (std::size_t i = 0; i < s.plane_count; ++i) {
      d.fds.at(i) = s.planes.at(i).duped_fd;
      d.offsets.at(i) = s.planes.at(i).offset;
      d.pitches.at(i) = s.planes.at(i).pitch;
    }
    return d;
  }
  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 private:
  explicit ExternalDmaBufRing(std::optional<std::chrono::nanoseconds> fence_deadline)
      : presenter_(fence_deadline) {}

  // One ring slot's imported kernel state — see detail/dmabuf_slot.hpp.
  using SlotRecord = detail::DmaBufSlot;

  /// Drop FB + GEM handles for every slot (keeps duped fds). Idempotent.
  void teardown_kernel_state() noexcept;
  void close_duped_fds() noexcept;
  void fire_release(std::size_t slot, std::optional<drm::sync::SyncFence> release_fence) noexcept;

  int fd_{-1};
  SourceFormat format_{};
  std::vector<SlotRecord> slots_;  // fixed at create(); slot index is the presenter key
  OnSlotRelease on_release_;

  // The presentation state machine (token map, idle-hold, damage carry, fence
  // deadline) — keyed by slot index. See detail/external_ring_core.hpp.
  detail::RingPresenter presenter_;
};

}  // namespace drm::scene
