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
  auto result = drm::vulkan::Display::create();
  if (!result) {
    std::println(stderr, "Failed to create Vulkan display (Vulkan may not be available)");
    return EXIT_FAILURE;
  }
  auto& display = *result;

  std::println("Vulkan displays: {}", display.displays().size());
  for (const auto& d : display.displays()) {
    std::println("  Display '{}': {}x{} (handle=0x{:x})", d.name, d.width, d.height,
                 d.display_handle);

    auto planes = display.planes_for_display(d.display_handle);
    for (const auto* p : planes) {
      std::println("    Plane {}: stack_index={}", p->plane_index, p->current_stack_index);
    }
  }

  std::println("\nVulkan display planes: {}", display.planes().size());
  for (const auto& p : display.planes()) {
    std::println("  Plane {}: stack_index={}, supported_displays={}", p.plane_index,
                 p.current_stack_index, p.supported_displays.size());
  }

  return EXIT_SUCCESS;
}
