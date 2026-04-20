// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// vulkan_display — demonstrates VK_KHR_display integration.
//
// Enumerates Vulkan displays and display planes, showing how
// they can be cross-referenced with DRM planes.

#include "../logind_session.hpp"
#include "drm-cxx/detail/format.hpp"
#include "vulkan/display.hpp"

#include <cstdlib>

int main() {
  // See atomic_modeset for why we claim a logind session.
  auto logind = drm::examples::LogindSession::open();

  const auto result = drm::vulkan::Display::create();
  if (!result) {
    drm::println(stderr, "Failed to create Vulkan display (Vulkan may not be available)");
    return EXIT_FAILURE;
  }
  const auto& display = *result;

  drm::println("Vulkan displays: {}", display.displays().size());
  for (const auto& [display_handle, name, width, height] : display.displays()) {
    drm::println("  Display '{}': {}x{} (handle=0x{:x})", name, width, height, display_handle);

    auto planes = display.planes_for_display(display_handle);
    for (const auto* p : planes) {
      drm::println("    Plane {}: stack_index={}", p->plane_index, p->current_stack_index);
    }
  }

  drm::println("\nVulkan display planes: {}", display.planes().size());
  for (const auto& [plane_index, current_stack_index, supported_displays] : display.planes()) {
    drm::println("  Plane {}: stack_index={}, supported_displays={}", plane_index,
                 current_stack_index, supported_displays.size());
  }

  return EXIT_SUCCESS;
}
