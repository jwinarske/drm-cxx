// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "drm_surface.hpp"

namespace drm::vulkan {

DrmSurface::~DrmSurface() = default;
DrmSurface::DrmSurface(DrmSurface&&) noexcept = default;
DrmSurface& DrmSurface::operator=(DrmSurface&&) noexcept = default;

uint64_t DrmSurface::surface_handle() const noexcept {
  return surface_;
}

}  // namespace drm::vulkan
