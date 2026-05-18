// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// nvbufsurface_source.hpp — LayerBufferSource backed by an NVIDIA L4T
// NvBufSurface (Jetson NVMM).
//
// Why this exists: on Jetson L4T, DRM `dumb` buffer mmap is
// write-combined — fine for producer-side streaming stores (Blend2D
// hits peak WC throughput), catastrophic for any consumer-side read
// (~250 ns/px effective for NEON loads vs ~5 ns/px theoretical
// cached). The compositor fallback inside `LayerScene::compose_unassigned`
// has to read every source layer's pixels, and large source buffers
// drag commit times into the tens of milliseconds.
//
// NvBufSurface is NVIDIA's own buffer-management layer used internally
// by DeepStream, nvarguscamerasrc, nvv4l2h264enc, etc. On Jetson with
// `NVBUF_MEM_SURFACE_ARRAY`, the CPU-side mmap is cached (with the
// usual `SyncFor{Cpu,Device}` discipline). The underlying NVMM
// allocation also exports a regular Linux dma-buf fd, which drm-cxx
// PRIME-imports + AddFB2's just like any other linear DMABUF — so the
// same surface scans out from a KMS plane without an extra copy.
//
// Net: cluster_sim's instruments-layer composition (and any future
// CPU compose path) reads source pixels at cached speeds, and the
// scene still scans out via the standard `BindingModel::SceneSubmitsFbId`
// contract.
//
// Build is gated on `DRM_CXX_HAS_NVBUFSURFACE`: present only on Jetson
// L4T systems with `jetson_multimedia_api` headers and
// `libnvbufsurface.so` available. Every other platform sees this
// header export an empty namespace and reaches for `DumbBufferSource`
// or `GbmBufferSource` as before.

#pragma once

#if DRM_CXX_HAS_NVBUFSURFACE

#include "buffer_source.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <memory>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// `LayerBufferSource` backed by a single NvBufSurface (Jetson NVMM).
/// CPU writes pixels through `map(MapAccess)` against a *cached*
/// mapping; the scene returns the same cached FB ID on every
/// `acquire()`. Format support today is XRGB8888 / ARGB8888 (BGRx and
/// BGRA in NvBuf terms) — extend with more entries in the
/// fourcc-to-NvBufColorFormat table when callers need them.
///
/// The dma-buf fd that backs the NvBufSurface is PRIME-imported into a
/// DRM GEM handle + framebuffer at `create()` time; teardown removes
/// the FB, closes the GEM handle, and destroys the NvBufSurface. The
/// fd itself belongs to the NvBufSurface allocation and is closed by
/// its destructor.
class NvBufSurfaceSource : public LayerBufferSource {
 public:
  /// Allocate a single NvBufSurface of the given size and DRM format,
  /// map it for CPU access, PRIME-import the underlying dma-buf into a
  /// scanout-ready DRM framebuffer.
  [[nodiscard]] static drm::expected<std::unique_ptr<NvBufSurfaceSource>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format);

  NvBufSurfaceSource(const NvBufSurfaceSource&) = delete;
  NvBufSurfaceSource& operator=(const NvBufSurfaceSource&) = delete;
  NvBufSurfaceSource(NvBufSurfaceSource&&) = delete;
  NvBufSurfaceSource& operator=(NvBufSurfaceSource&&) = delete;
  ~NvBufSurfaceSource() override;

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return format_; }
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> map(
      drm::MapAccess access) override;
  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 public:
  // Impl is declared `public` only so the unmap thunk that
  // `BufferMapping` stores as a plain `void(void*)` can reach the cache-
  // sync flag without a brittle friend / extern-C declaration. Treat
  // its layout as private — it changes without notice.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;

  explicit NvBufSurfaceSource(std::unique_ptr<Impl> impl) noexcept;

  SourceFormat format_{};
};

}  // namespace drm::scene

#endif  // DRM_CXX_HAS_NVBUFSURFACE
