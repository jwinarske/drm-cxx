// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// external_dma_buf_pool.hpp — LayerBufferSource over a *dynamically-populated*
// pool of caller-owned, externally-allocated DMA-BUFs, for a producer that
// reveals its buffer set lazily and identifies each frame by a stable key.
//
// Where ExternalDmaBufRing fixes its slot set (count + geometry) at create() and
// the producer submits by slot *index*, the pool imports a buffer the first time
// it is submitted (by opaque `buffer_key`) and reuses the cached fb_id on every
// later submit of that key — the DmaBufSourceCache lazy-import semantics folded
// behind the ring's submit/acquire/release presentation machine. MPP-style
// stateful decoders (V4L2 CAPTURE, Rockchip MPP) that recycle an internal pool
// and hand out fds per frame are the motivating producer.
//
// It shares both halves of the extracted ring core: detail::DmaBufSlot for the
// per-buffer PRIME import / teardown, and detail::RingPresenter for the
// producer->commit handoff, acquisition tokens + deferred release, idle
// hold-last-frame, damage carry, and the fence deadline. The presenter is keyed
// by `buffer_key`; the pool maps that key back to its cached fb_id.
//
// Threading: submit() runs on the producer thread (and performs the first-sight
// import — not alloc-free, AddFB2 can fail) while the scene calls
// acquire()/release() on its commit thread. The import map is guarded by
// slots_mu_; the pending-frame handoff is guarded inside the presenter. The two
// locks are never held nested, so there is no ordering hazard.
//
// v1 scope: lazy import + cached fb_id + presentation + session re-import. Bounded
// LRU eviction and generation reset are separate follow-ups.

#pragma once

#include "buffer_source.hpp"  // LayerBufferSource, ExternalPlaneInfo
#include "detail/dmabuf_slot.hpp"
#include "detail/external_ring_core.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

class ExternalDmaBufPool : public LayerBufferSource {
 public:
  /// Buffer `buffer_key` left scanout. `release_fence`, when present, signals
  /// GPU-side once that buffer is safe to render into again (the OUT_FENCE of the
  /// commit that displaced it); nullopt means the callback edge itself is the
  /// "buffer free" signal. Fires on the scene's commit thread.
  using OnBufferRelease = std::function<void(std::uintptr_t buffer_key,
                                             std::optional<drm::sync::SyncFence> release_fence)>;

  struct Options {
    OnBufferRelease on_release;
    /// Fault isolation: same CPU fence pre-wait / hold-last-frame contract as
    /// ExternalDmaBufRing::Options::fence_deadline. See RingPresenter.
    std::optional<std::chrono::nanoseconds> fence_deadline;
    /// Bound on the number of imported buffers kept cached. Once exceeded, the
    /// least-recently-submitted *idle* entries are evicted on acquire() —
    /// eviction of a buffer still referenced by an in-flight commit is deferred
    /// until it retires. Covers producers with unstable buffer identities so the
    /// import map can't grow without bound. Must be >= 1.
    std::size_t max_pool{32};
  };

  /// Create an empty pool for `dev` at the generation geometry
  /// `width`/`height`/`drm_fourcc`/`modifier`. Buffers are imported lazily on
  /// first submit(). Returns errc::invalid_argument on a zero dimension/format
  /// or errc::bad_file_descriptor on a dead device.
  [[nodiscard]] static drm::expected<std::unique_ptr<ExternalDmaBufPool>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_fourcc,
      std::uint64_t modifier, Options options);

  /// Overload with default Options (no callback, no deadline, max_pool 32). Split
  /// out because a DMI-bearing aggregate can't form an in-class default argument.
  [[nodiscard]] static drm::expected<std::unique_ptr<ExternalDmaBufPool>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_fourcc,
      std::uint64_t modifier);

  ExternalDmaBufPool(const ExternalDmaBufPool&) = delete;
  ExternalDmaBufPool& operator=(const ExternalDmaBufPool&) = delete;
  ExternalDmaBufPool(ExternalDmaBufPool&&) = delete;
  ExternalDmaBufPool& operator=(ExternalDmaBufPool&&) = delete;
  ~ExternalDmaBufPool() override;

  /// Producer hands in the buffer to scan out next, identified by `buffer_key`,
  /// with its dma-buf `planes` and (optionally) a render-done fence + damage.
  /// The first submit of a key imports its planes and caches the fb_id; later
  /// submits of the same key reuse it (the `planes` are then ignored). An import
  /// failure logs (DRM_EXT_DMABUF_DEBUG) and skips the frame — the presenter
  /// holds the last good buffer rather than blanking the layer. Thread-safe vs
  /// acquire()/release(). `planes` must be 1..4 with each fd >= 0 and pitch != 0.
  void submit(std::uintptr_t buffer_key, drm::span<const ExternalPlaneInfo> planes,
              std::optional<drm::sync::SyncFence> acquire = std::nullopt,
              drm::span<const DamageRect> damage = {}) noexcept;

  /// Number of buffers currently imported/cached (observability + tests).
  [[nodiscard]] std::size_t cached_count() const noexcept;

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  void release_with_fence(AcquiredBuffer acquired,
                          std::optional<drm::sync::SyncFence> release_fence) noexcept override;
  [[nodiscard]] bool wants_release_fence() const noexcept override {
    return static_cast<bool>(on_release_);
  }
  [[nodiscard]] bool has_fresh_content() const noexcept override {
    return presenter_.has_fresh_frame();
  }
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return format_; }
  [[nodiscard]] drm::expected<DmaBufDesc, std::error_code> export_dma_buf() override;
  void on_session_paused() noexcept override {}
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 private:
  explicit ExternalDmaBufPool(std::optional<std::chrono::nanoseconds> fence_deadline,
                              std::size_t max_pool)
      : max_pool_(max_pool < 1 ? 1 : max_pool), presenter_(fence_deadline) {}

  void fire_release(std::uintptr_t buffer_key,
                    std::optional<drm::sync::SyncFence> release_fence) noexcept;

  // Move `buffer_key` to most-recently-used. Caller holds slots_mu_.
  void touch_lru(std::uintptr_t buffer_key);
  // Tear down + forget the buffer under `buffer_key`. Caller holds slots_mu_.
  void evict_locked(std::uintptr_t buffer_key) noexcept;
  // Evict least-recently-used idle buffers until at most max_pool_ remain,
  // skipping any the presenter still references. Caller holds slots_mu_ and runs
  // on the commit thread (so the presenter's liveness view is stable).
  void prune_locked() noexcept;

  int fd_{-1};
  SourceFormat format_{};
  std::size_t max_pool_{32};
  OnBufferRelease on_release_;

  // buffer_key -> imported buffer (dma_buf planes + cached fb_id). Guarded by
  // slots_mu_ because submit() imports on the producer thread while acquire() /
  // export_dma_buf() read on the commit thread. `lru_` orders keys
  // least-recently-submitted (front) to most (back); `lru_pos_` is its index.
  mutable std::mutex slots_mu_;
  std::unordered_map<std::uintptr_t, detail::DmaBufSlot> slots_;
  std::list<std::uintptr_t> lru_;
  std::unordered_map<std::uintptr_t, std::list<std::uintptr_t>::iterator> lru_pos_;

  // Presentation state machine, keyed by buffer_key. See external_ring_core.hpp.
  detail::RingPresenter presenter_;
};

}  // namespace drm::scene
