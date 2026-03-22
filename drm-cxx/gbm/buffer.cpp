// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "buffer.hpp"

#include <cerrno>
#include <gbm.h>

namespace drm::gbm {

Buffer::Buffer(struct gbm_bo* bo, struct gbm_surface* surf) noexcept
  : bo_(bo), surf_(surf) {}

Buffer::~Buffer() {
  if (bo_ && surf_) {
    // Buffer was locked from a surface — release it back
    gbm_surface_release_buffer(surf_, bo_);
  } else if (bo_) {
    gbm_bo_destroy(bo_);
  }
}

Buffer::Buffer(Buffer&& other) noexcept
  : bo_(other.bo_), surf_(other.surf_) {
  other.bo_ = nullptr;
  other.surf_ = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    if (bo_ && surf_) {
      gbm_surface_release_buffer(surf_, bo_);
    } else if (bo_) {
      gbm_bo_destroy(bo_);
    }
    bo_ = other.bo_;
    surf_ = other.surf_;
    other.bo_ = nullptr;
    other.surf_ = nullptr;
  }
  return *this;
}

struct gbm_bo* Buffer::raw() const noexcept { return bo_; }

uint32_t Buffer::handle() const noexcept {
  if (!bo_) return 0;
  return gbm_bo_get_handle(bo_).u32;
}

uint32_t Buffer::stride() const noexcept {
  if (!bo_) return 0;
  return gbm_bo_get_stride(bo_);
}

uint32_t Buffer::width() const noexcept {
  if (!bo_) return 0;
  return gbm_bo_get_width(bo_);
}

uint32_t Buffer::height() const noexcept {
  if (!bo_) return 0;
  return gbm_bo_get_height(bo_);
}

uint32_t Buffer::format() const noexcept {
  if (!bo_) return 0;
  return gbm_bo_get_format(bo_);
}

std::expected<int, std::error_code> Buffer::fd() const {
  if (!bo_) {
    return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int dma_fd = gbm_bo_get_fd(bo_);
  if (dma_fd < 0) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }
  return dma_fd;
}

} // namespace drm::gbm
