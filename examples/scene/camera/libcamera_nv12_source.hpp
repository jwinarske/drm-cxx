// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// libcamera_nv12_source.hpp — multi-FB scanout source over libcamera's
// rotating buffer pool.
//
// libcamera's FrameBufferAllocator hands out a fixed pool of buffers
// (typically 4) and rotates them across requestCompleted callbacks.
// Each buffer has a unique dma-buf fd. To scan a frame out zero-copy
// the kernel needs a stable `fb_id` tied to that fd; minting a fresh
// FB per frame would churn `drmModeAddFB2WithModifiers` every commit.
//
// This source pre-mints one FB per buffer fd at construction (from
// the allocator's pool) and lets the example flip between them via
// `set_current_fd()` on each request. `acquire()` reports whichever
// fd is "current" — the scene re-emits FB_ID every frame anyway, so
// switching fb_ids is just a property write.
//
// Provenance constraints:
//   * amdgpu DC refuses any dma-buf whose ops aren't amdgpu's; UVC
//     vmalloc-backed dma-bufs fail `drmModeAddFB2WithModifiers` with
//     EINVAL. Callers must treat `create()` returning nullptr as the
//     signal to fall back to a tier whose buffers are amdgpu-owned
//     (VAAPI surfaces) or CPU-mapped (DumbBufferSource).
//   * i915 integrated (no LMEM, every iGPU through Gen ≥ 9) accepts
//     foreign dma-bufs as long as the format / modifier match the
//     plane's IN_FORMATS — NV12 + LINEAR works.
//   * i915 discrete (Arc/BMG, has LMEM) refuses with EREMOTE
//     ("framebuffer must reside in local memory").
//   * Platforms with CMA-backed capture (RPi unicam, RK3588 ISP1)
//     accept the imports cleanly.
//
// Uncompositable: `map()` returns `function_not_supported`. The
// negotiator should only pick this tier when the plane budget can
// host the layer directly; overflow callers must use a CPU-mappable
// tier (libyuv repack) instead.

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <cstdint>
#include <memory>
#include <system_error>
#include <unordered_map>

namespace drm {
class Device;
}  // namespace drm

namespace drm::examples::camera {

/// Plane descriptor for one of NV12's two layers within a buffer.
struct NV12PlaneInfo {
  std::uint32_t offset;
  std::uint32_t pitch;
};

class LibcameraNv12Source : public drm::scene::LayerBufferSource {
 public:
  /// Build the source with no FBs pre-cached; the caller registers
  /// each libcamera buffer fd via `register_fd()` once the allocator
  /// has handed them out. The first `register_fd()` that succeeds
  /// confirms the platform accepts foreign dma-bufs at AddFB2 time;
  /// the first that fails is the signal to abandon this tier entirely
  /// (the source has nothing to scan out yet).
  ///
  /// `dev` is borrowed (typical scene-device lifetime). `width` /
  /// `height` / `drm_fourcc` / `modifier` describe the per-frame
  /// buffers; every fd handed to `register_fd()` must share that
  /// layout. `y` and `uv` describe NV12's two planes' offsets and
  /// pitches within each buffer.
  [[nodiscard]] static std::unique_ptr<LibcameraNv12Source> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_fourcc,
      std::uint64_t modifier, NV12PlaneInfo y, NV12PlaneInfo uv) noexcept;

  ~LibcameraNv12Source() override;
  LibcameraNv12Source(const LibcameraNv12Source&) = delete;
  LibcameraNv12Source& operator=(const LibcameraNv12Source&) = delete;
  LibcameraNv12Source(LibcameraNv12Source&&) = delete;
  LibcameraNv12Source& operator=(LibcameraNv12Source&&) = delete;

  /// Mint an FB for `fd` if not already cached, and return whether
  /// the mint succeeded. Pass every libcamera buffer's fd through
  /// here once at slot setup; subsequent calls with the same fd are
  /// cache hits. Failure (e.g. amdgpu dma-buf provenance EINVAL,
  /// i915 discrete EREMOTE) is reported by `false`; the caller drops
  /// this source and falls back to another tier.
  [[nodiscard]] bool register_fd(int fd) noexcept;

  /// Switch which cached FB `acquire()` will return. Called from
  /// drain_slot when a request completes and we know which buffer
  /// (by fd) it landed in. Returns false if the fd was never
  /// successfully registered (treat as a per-frame skip; the scene
  /// will keep the prior fb_id scanning out).
  [[nodiscard]] bool set_current_fd(int fd) noexcept;

  // ── LayerBufferSource overrides ────────────────────────────────

  [[nodiscard]] drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override;
  void release(drm::scene::AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] drm::scene::BindingModel binding_model() const noexcept override;
  [[nodiscard]] drm::scene::SourceFormat format() const noexcept override;
  // Inherits the default `map()` returning `function_not_supported`.
  // This source is uncompositable; the negotiator/budget gate must
  // route overflow to a CPU-mappable tier.

 private:
  LibcameraNv12Source() = default;

  /// Tear down every FB + GEM handle + duped fd. Idempotent.
  void destroy_state() noexcept;

  struct FbEntry {
    int duped_fd{-1};
    std::uint32_t gem_handle{0};
    std::uint32_t fb_id{0};
  };

  int drm_fd_{-1};
  drm::scene::SourceFormat fmt_{};
  NV12PlaneInfo y_{};
  NV12PlaneInfo uv_{};
  std::unordered_map<int, FbEntry> cache_{};
  std::uint32_t current_fb_id_{0};
};

}  // namespace drm::examples::camera
