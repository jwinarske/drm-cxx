// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "drm_surface.hpp"

#include <cstdint>

namespace drm::vulkan {

DrmSurface::DrmSurface(DrmSurface&&) noexcept = default;
DrmSurface& DrmSurface::operator=(DrmSurface&&) noexcept = default;

uint64_t DrmSurface::surface_handle() const noexcept {
  return surface_;
}

}  // namespace drm::vulkan
