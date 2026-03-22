// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0
//
// vulkan_display — demonstrates VK_KHR_display integration.
//
// Enumerates Vulkan displays and display planes, showing how
// they can be cross-referenced with DRM planes.

#include "vulkan/display.hpp"

#include <cstdlib>
#include <print>

int main() {
  const auto result = drm::vulkan::Display::create();
  if (!result) {
    std::println(stderr, "Failed to create Vulkan display (Vulkan may not be available)");
    return EXIT_FAILURE;
  }
  const auto& display = *result;

  std::println("Vulkan displays: {}", display.displays().size());
  for (const auto& [display_handle, name, width, height] : display.displays()) {
    std::println("  Display '{}': {}x{} (handle=0x{:x})", name, width, height, display_handle);

    for (auto planes = display.planes_for_display(display_handle); const auto* p : planes) {
      std::println("    Plane {}: stack_index={}", p->plane_index, p->current_stack_index);
    }
  }

  std::println("\nVulkan display planes: {}", display.planes().size());
  for (const auto& [plane_index, current_stack_index, supported_displays] : display.planes()) {
    std::println("  Plane {}: stack_index={}, supported_displays={}", plane_index,
                 current_stack_index, supported_displays.size());
  }

  return EXIT_SUCCESS;
}
