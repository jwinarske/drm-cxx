// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <vector>

namespace drm::vulkan {

struct DisplayInfo {
  uint64_t display_handle{};  // VkDisplayKHR
  std::string name;
  uint32_t width{};
  uint32_t height{};
};

struct DisplayPlaneInfo {
  uint32_t plane_index{};
  uint32_t current_stack_index{};
  std::vector<uint64_t> supported_displays;  // VkDisplayKHR handles
};

class Display {
public:
  // Create by enumerating VkDisplayKHR displays on a physical device.
  // Loads Vulkan dynamically; fails gracefully if unavailable.
  static std::expected<Display, std::error_code> create();

  [[nodiscard]] const std::vector<DisplayInfo>& displays() const noexcept;
  [[nodiscard]] const std::vector<DisplayPlaneInfo>& planes() const noexcept;

  // Find display planes compatible with a given DRM CRTC.
  [[nodiscard]] std::vector<const DisplayPlaneInfo*>
    planes_for_display(uint64_t display_handle) const;

  ~Display();
  Display(Display&&) noexcept;
  Display& operator=(Display&&) noexcept;
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

private:
  Display() = default;

  std::vector<DisplayInfo> displays_;
  std::vector<DisplayPlaneInfo> planes_;

  // Vulkan handles (stored as opaque pointers to avoid header dependency)
  void* instance_{};
  void* physical_device_{};
};

} // namespace drm::vulkan
