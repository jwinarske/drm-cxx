// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "surface.hpp"

#include "device.hpp"
#include "gbm/buffer.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <gbm.h>

#include <cerrno>
#include <cstdint>
#include <system_error>

namespace drm::gbm {

Surface::Surface(struct gbm_surface* surf) noexcept : surf_(surf) {}

Surface::~Surface() {
  if (surf_ != nullptr) {
    gbm_surface_destroy(surf_);
  }
}

Surface::Surface(Surface&& other) noexcept : surf_(other.surf_) {
  other.surf_ = nullptr;
}

Surface& Surface::operator=(Surface&& other) noexcept {
  if (this != &other) {
    if (surf_ != nullptr) {
      gbm_surface_destroy(surf_);
    }
    surf_ = other.surf_;
    other.surf_ = nullptr;
  }
  return *this;
}

drm::expected<Surface, std::error_code> Surface::create(GbmDevice& dev, uint32_t width,
                                                        uint32_t height, uint32_t format,
                                                        uint32_t flags) {
  auto* surf = gbm_surface_create(dev.raw(), width, height, format, flags);
  if (surf == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  return Surface(surf);
}

struct gbm_surface* Surface::raw() const noexcept {
  return surf_;
}

drm::expected<Buffer, std::error_code> Surface::lock_front_buffer() {
  if (surf_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  auto* bo = gbm_surface_lock_front_buffer(surf_);
  if (bo == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  return Buffer(bo, surf_);
}

bool Surface::has_free_buffers() const noexcept {
  if (surf_ == nullptr) {
    return false;
  }
  return gbm_surface_has_free_buffers(surf_) != 0;
}

}  // namespace drm::gbm
