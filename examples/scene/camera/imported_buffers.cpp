// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// imported_buffers.cpp — dma-heap (Mode B) and GBM (Mode C) capture buffers.

#include "imported_buffers.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>

#include <gbm.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <libcamera/base/shared_fd.h>
#include <libcamera/framebuffer.h>
#include <libcamera/stream.h>
#include <linux/dma-heap.h>
#include <memory>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace drm::examples::camera {

const char* import_mode_label(ImportMode mode) noexcept {
  switch (mode) {
    case ImportMode::DmaHeapSystem:
      return "dma-heap(system)";
    case ImportMode::DmaHeapCma:
      return "dma-heap(cma)";
    case ImportMode::Gbm:
      return "gbm";
  }
  return "?";
}

namespace {

// Open the first dma-heap device that exists for `mode`. The CMA heap's name is
// not standardized: RPi exports `linux,cma` and `default_cma_region`; other
// kernels use plain `cma`. Try them in order. Returns -1 (with errno) on miss.
int open_dma_heap(ImportMode mode) noexcept {
  static constexpr std::array<const char*, 1> k_system = {"/dev/dma_heap/system"};
  static constexpr std::array<const char*, 3> k_cma = {
      "/dev/dma_heap/linux,cma", "/dev/dma_heap/default_cma_region", "/dev/dma_heap/cma"};
  const auto try_open = [](const auto& names) -> int {
    for (const char* name : names) {
      const int fd = ::open(name, O_RDWR | O_CLOEXEC);
      if (fd >= 0) {
        return fd;
      }
    }
    return -1;
  };
  return mode == ImportMode::DmaHeapSystem ? try_open(k_system) : try_open(k_cma);
}

// Allocate one dma-buf of `len` bytes from an open dma-heap. -1 on failure.
int dma_heap_alloc(int heap_fd, std::size_t len) noexcept {
  dma_heap_allocation_data data{};
  data.len = len;
  data.fd_flags = O_RDWR | O_CLOEXEC;
  if (::ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) != 0) {
    return -1;
  }
  return static_cast<int>(data.fd);
}

}  // namespace

struct ImportedBufferPool::Impl {
  int heap_fd{-1};             // Mode B: open dma-heap device
  gbm_device* gbm{nullptr};    // Mode C: GBM device on the DRM fd
  std::vector<gbm_bo*> bos;    // Mode C: allocated bos
  std::vector<int> owned_fds;  // dma-buf fds we own (dma-heap; gbm exports)

  ~Impl() {
    for (const int fd : owned_fds) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    for (gbm_bo* bo : bos) {
      if (bo != nullptr) {
        gbm_bo_destroy(bo);
      }
    }
    if (gbm != nullptr) {
      gbm_device_destroy(gbm);
    }
    if (heap_fd >= 0) {
      ::close(heap_fd);
    }
  }
};

namespace {

// Wrap a dma-buf fd as a single-plane libcamera FrameBuffer of `len` bytes. The
// RPi ISP NV12 output is single-planar (one contiguous dma-buf); the scanout
// source derives the UV offset from the stride, so one plane is enough. SharedFD
// dups the fd, so the pool keeps ownership of the original.
std::unique_ptr<libcamera::FrameBuffer> wrap_fb(int fd, std::size_t len) {
  std::vector<libcamera::FrameBuffer::Plane> planes;
  libcamera::FrameBuffer::Plane p;
  p.fd = libcamera::SharedFD(fd);
  p.offset = 0;
  p.length = static_cast<unsigned int>(len);
  planes.push_back(std::move(p));
  return std::make_unique<libcamera::FrameBuffer>(planes);
}

}  // namespace

std::unique_ptr<ImportedBufferPool> ImportedBufferPool::create(
    ImportMode mode, const drm::Device& dev, const libcamera::StreamConfiguration& cfg,
    unsigned count) noexcept try {
  if (count == 0 || cfg.frameSize == 0) {
    return nullptr;
  }
  auto pool = std::unique_ptr<ImportedBufferPool>(new ImportedBufferPool());
  pool->impl_ = std::make_unique<Impl>();
  Impl& impl = *pool->impl_;

  if (mode == ImportMode::Gbm) {
    impl.gbm = gbm_create_device(dev.fd());
    if (impl.gbm == nullptr) {
      drm::println(stderr, "imported_buffers: gbm_create_device failed");
      return nullptr;
    }
    for (unsigned i = 0; i < count; ++i) {
      gbm_bo* bo = gbm_bo_create(impl.gbm, cfg.size.width, cfg.size.height, GBM_FORMAT_NV12,
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
      if (bo == nullptr) {
        drm::println(stderr, "imported_buffers: gbm_bo_create(NV12) failed");
        return nullptr;
      }
      impl.bos.push_back(bo);
      const int fd = gbm_bo_get_fd(bo);
      if (fd < 0) {
        drm::println(stderr, "imported_buffers: gbm_bo_get_fd failed");
        return nullptr;
      }
      impl.owned_fds.push_back(fd);
      // GBM NV12 is a single bo; size = stride * height * 3/2 (LINEAR). Use the
      // bo's own stride so the FB length matches the allocation.
      const std::size_t len =
          static_cast<std::size_t>(gbm_bo_get_stride(bo)) * cfg.size.height * 3U / 2U;
      pool->buffers_.push_back(wrap_fb(fd, len));
    }
    return pool;
  }

  // Mode B: dma-heap.
  impl.heap_fd = open_dma_heap(mode);
  if (impl.heap_fd < 0) {
    drm::println(stderr, "imported_buffers: no dma-heap for {} ({})", import_mode_label(mode),
                 std::strerror(errno));
    return nullptr;
  }
  for (unsigned i = 0; i < count; ++i) {
    const int fd = dma_heap_alloc(impl.heap_fd, cfg.frameSize);
    if (fd < 0) {
      drm::println(stderr, "imported_buffers: DMA_HEAP_IOCTL_ALLOC({} B) failed: {}", cfg.frameSize,
                   std::strerror(errno));
      return nullptr;
    }
    impl.owned_fds.push_back(fd);
    pool->buffers_.push_back(wrap_fb(fd, cfg.frameSize));
  }
  return pool;
} catch (...) {
  // The factory's contract is nullptr-on-failure; a bad_alloc from the pool /
  // FrameBuffer construction is just another failure, not a demo crash.
  return nullptr;
}

ImportedBufferPool::~ImportedBufferPool() = default;

}  // namespace drm::examples::camera
