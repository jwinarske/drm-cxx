// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Private (not installed): drop a GEM handle with DRM_IOCTL_GEM_CLOSE.
//
// libdrm's drmCloseBufferHandle does the same, but it came in 2.4.108; a
// binary that calls it fails to load (or, bound lazily, at the first close)
// on a system with an older libdrm. drmIoctl is in every libdrm.

#pragma once

#include <xf86drm.h>

#include <cstdint>

namespace drm::detail {

// Returns drmIoctl's rc; errno is left as the ioctl set it.
inline int close_gem_handle(int fd, std::uint32_t handle) noexcept {
  drm_gem_close gc{};
  gc.handle = handle;
  return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
}

}  // namespace drm::detail
