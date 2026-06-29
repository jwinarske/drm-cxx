// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// external_dma_buf_source.hpp — LayerBufferSource that wraps a
// caller-owned, externally-allocated DMA-BUF and registers it as a KMS
// framebuffer for scanout.
//
// Motivating consumer: zero-copy capture pipelines (libcamera, V4L2,
// accel/NPU producers) where the buffer is allocated upstream and
// reaches the scene as a (fd, format, modifier, width, height) tuple
// rather than as a buffer the scene allocated. The source dups the
// dma-buf fds at create() time so its lifetime is independent of the
// caller's; the caller is free to close their fds the moment the
// factory returns.
//
// Format scope: 1 to 4 planes, any modifier. create() forwards whatever
// modifier the caller supplies straight to drmModeAddFB2WithModifiers
// (LINEAR/INVALID skip DRM_MODE_FB_MODIFIERS; anything else passes through
// verbatim) — the kernel validates against its driver tables and rejects an
// unsupported tiling at AddFB2 time, so the kernel is the ground truth here,
// not this type. A caller that wants to know whether a tiled/compressed
// modifier (AFBC, block-linear, V3D, X/Y_TILED) will land should consult
// drm::fmt::FormatTable / a DRM_MODE_ATOMIC_TEST_ONLY commit up front rather
// than discover it at AddFB2.
//
// This type is the simple, *single-use* path (one buffer, one cached fb_id;
// see below). For a *rotating* external producer that hands in a fresh tiled
// slot per frame — and wants validate-not-negotiate against a plane's
// IN_FORMATS plus per-frame damage / fence handoff — use
// scene/external_dma_buf_ring.hpp (ExternalDmaBufRing).
//
// Single-use semantics. The source caches one fb_id at create() time
// and returns it from every acquire(). on_release fires exactly once
// — from release() the first time the scene retires the buffer, or
// from the destructor if the source is torn down without ever
// reaching release(). Callers use it to re-queue the upstream Request
// that owns the foreign buffer.

#pragma once

#include "buffer_source.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// Per-plane shape of an externally-allocated DMA-BUF. `fd` is the
/// dma-buf descriptor (caller-owned; the source dups it). `offset` and
/// `pitch` are the plane's offset into that fd and its row stride in
/// bytes — V4L2 / libcamera report these directly.
struct ExternalPlaneInfo {
  int fd{-1};
  std::uint32_t offset{0};
  std::uint32_t pitch{0};
};

/// `LayerBufferSource` wrapping a caller-owned DMA-BUF as a
/// scanout-ready KMS framebuffer. See file comment for the full
/// contract.
class ExternalDmaBufSource : public LayerBufferSource {
 public:
  /// Wrap the given dma-buf planes as a KMS FB on `dev`.
  ///
  /// The factory:
  ///   1. validates inputs (non-zero dimensions, non-zero fourcc,
  ///      LINEAR/INVALID modifier, 1..4 planes each with a non-negative
  ///      fd and non-zero pitch),
  ///   2. duplicates each plane's fd so the source's lifetime is
  ///      independent of the caller's,
  ///   3. resolves each duped fd into a GEM handle on `dev` via
  ///      drmPrimeFDToHandle,
  ///   4. binds those handles to a KMS FB via
  ///      drmModeAddFB2WithModifiers, caching the fb_id.
  ///
  /// Plane count vs. fourcc consistency (NV12 wants 2, YUV420 wants 3,
  /// etc.) is left to the kernel — drmModeAddFB2WithModifiers returns
  /// EINVAL on a mismatch and the factory propagates that.
  ///
  /// On any failure the partial state (duped fds, GEM handles) is
  /// unwound before returning. `on_release` is *not* fired on failure —
  /// the caller can re-queue / drop the upstream buffer themselves.
  [[nodiscard]] static drm::expected<std::unique_ptr<ExternalDmaBufSource>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format,
      std::uint64_t modifier, drm::span<const ExternalPlaneInfo> planes,
      std::function<void()> on_release = {});

  ExternalDmaBufSource(const ExternalDmaBufSource&) = delete;
  ExternalDmaBufSource& operator=(const ExternalDmaBufSource&) = delete;
  ExternalDmaBufSource(ExternalDmaBufSource&&) = delete;
  ExternalDmaBufSource& operator=(ExternalDmaBufSource&&) = delete;
  ~ExternalDmaBufSource() override;

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return format_; }
  // map() inherits the base default — foreign sources do not expose
  // a CPU mapping. Layers backed by an ExternalDmaBufSource that the
  // allocator can't place on a hardware plane will be dropped this
  // frame; the composition fallback cannot rescue them.
  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

  // Stash a render-done sync_file the next acquire() hands back as the buffer's
  // acquire fence (the scene wires it to the plane's IN_FENCE_FD, or CPU-waits
  // it on drivers without that property). Used by GPU producers
  // (VkScanoutProducer) that render asynchronously instead of CPU-blocking.
  // Replaces any previously-stashed, not-yet-acquired fence.
  void set_acquire_fence(drm::sync::SyncFence fence) noexcept { pending_fence_ = std::move(fence); }

 private:
  ExternalDmaBufSource() = default;

  /// Drop the FB and GEM handles bound to fd_, leaving the duped
  /// dma-buf fds intact so on_session_resumed can re-import them.
  /// Idempotent. No-op when fd_ is already -1.
  void teardown_kernel_state() noexcept;

  /// Drop the duped dma-buf fds. Called from the destructor only.
  void close_duped_fds() noexcept;

  /// Fire on_release_ at most once. Called from release() and from the
  /// destructor.
  void fire_on_release_once() noexcept;

  static constexpr std::size_t k_max_planes = 4;

  struct PlaneRecord {
    int duped_fd{-1};
    std::uint32_t gem_handle{0};
    std::uint32_t offset{0};
    std::uint32_t pitch{0};
  };

  int fd_{-1};
  std::uint32_t fb_id_{0};
  std::array<PlaneRecord, k_max_planes> planes_{};
  std::size_t plane_count_{0};
  SourceFormat format_{};
  std::optional<drm::sync::SyncFence> pending_fence_;
  std::function<void()> on_release_;
  bool on_release_fired_{false};
};

}  // namespace drm::scene