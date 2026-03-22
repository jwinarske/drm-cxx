// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <system_error>

namespace drm::vulkan {

// Placeholder for VK_KHR_display surface creation.
// Full implementation requires a VkInstance and VkPhysicalDevice,
// which the Display class manages.
class DrmSurface {
 public:
  [[nodiscard]] uint64_t surface_handle() const noexcept;

  ~DrmSurface();
  DrmSurface(DrmSurface&&) noexcept;
  DrmSurface& operator=(DrmSurface&&) noexcept;
  DrmSurface(const DrmSurface&) = delete;
  DrmSurface& operator=(const DrmSurface&) = delete;

 private:
  DrmSurface() = default;
  friend class Display;
  uint64_t surface_{};
};

}  // namespace drm::vulkan
