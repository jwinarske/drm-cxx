// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// imported_buffers.hpp — application-allocated, scanout-capable capture buffers
// for the libcamera zero-copy tier.
//
// libcamera's FrameBufferAllocator (Mode A) allocates capture buffers from the
// V4L2 device's own MMAP pool. That memory is not always scanout-capable — on
// some ISP setups (Rockchip rkisp1) V4L2 MMAP yields buffers the display
// controller can't `AddFB2`. Two alternates let the application own the
// allocation so the buffers come from memory the display *can* scan out, then
// hand them to libcamera to fill:
//
//   * Mode B — dma-heap: allocate each buffer from `/dev/dma_heap/{system,cma}`
//     (`DMA_HEAP_IOCTL_ALLOC`). CMA-backed heaps give physically-contiguous
//     memory that a display controller with no IOMMU (RPi vc4) can scan out.
//   * Mode C — GBM: allocate a `gbm_bo` with `GBM_BO_USE_SCANOUT | ..._LINEAR`
//     so the buffer is GPU-driver-owned and thus scanout-capable on platforms
//     where only the GPU path reaches display memory (some i.MX setups).
//
// Both wrap the resulting dma-buf as a `libcamera::FrameBuffer`; the camera
// pipeline handler imports it (V4L2_MEMORY_DMABUF) when the request is queued,
// so the ISP writes straight into scanout memory — no copy. The buffers' fds
// then feed `LibcameraNv12Source::register_fd()` exactly like Mode A's.

#pragma once

#include <cstdint>
#include <libcamera/framebuffer.h>
#include <libcamera/stream.h>
#include <memory>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::examples::camera {

enum class ImportMode : std::uint8_t {
  DmaHeapSystem,  // /dev/dma_heap/system
  DmaHeapCma,     // /dev/dma_heap/{linux,cma | cma | default_cma_region}
  Gbm,            // gbm_bo, SCANOUT | LINEAR
};

[[nodiscard]] const char* import_mode_label(ImportMode mode) noexcept;

/// A pool of application-allocated, scanout-capable capture buffers, each
/// wrapped as a libcamera FrameBuffer for one stream. Owns the underlying
/// dma-heap fds / gbm_bos; the FrameBuffers hold dup'd fds (SharedFD), so the
/// pool must outlive the camera's use of them.
class ImportedBufferPool {
 public:
  /// Allocate `count` buffers sized for `cfg` from `mode`. `dev` is the DRM
  /// device the buffers must be scannable on (used as the GBM device for
  /// Mode C; unused for dma-heap). Returns nullptr on any failure — the caller
  /// falls back to the libcamera allocator (Mode A).
  [[nodiscard]] static std::unique_ptr<ImportedBufferPool> create(
      ImportMode mode, const drm::Device& dev, const libcamera::StreamConfiguration& cfg,
      unsigned count) noexcept;

  ~ImportedBufferPool();
  ImportedBufferPool(const ImportedBufferPool&) = delete;
  ImportedBufferPool& operator=(const ImportedBufferPool&) = delete;
  ImportedBufferPool(ImportedBufferPool&&) = delete;
  ImportedBufferPool& operator=(ImportedBufferPool&&) = delete;

  /// The FrameBuffers to hand libcamera (createRequest/addBuffer) and whose
  /// fds feed the scanout source. Same shape as `FrameBufferAllocator::buffers`.
  [[nodiscard]] const std::vector<std::unique_ptr<libcamera::FrameBuffer>>& buffers()
      const noexcept {
    return buffers_;
  }

 private:
  ImportedBufferPool() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;  // owns dma-heap fds / gbm device+bos
  std::vector<std::unique_ptr<libcamera::FrameBuffer>> buffers_;
};

}  // namespace drm::examples::camera
