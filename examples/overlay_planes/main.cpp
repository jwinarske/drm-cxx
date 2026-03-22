// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0
//
// overlay_planes — demonstrates the native plane allocator.
//
// Usage: overlay_planes [/dev/dri/cardN]
//
// Opens a DRM device, enumerates planes, creates virtual layers,
// and runs the allocator to assign layers to hardware planes.

#include "core/device.hpp"
#include "core/resources.hpp"
#include "modeset/atomic.hpp"
#include "planes/allocator.hpp"
#include "planes/plane_registry.hpp"

#include <cstdlib>
#include <print>

int main(int argc, char* argv[]) {
  const char* path = (argc > 1) ? argv[1] : "/dev/dri/card0";

  // Open DRM device
  auto dev_result = drm::Device::open(path);
  if (!dev_result) {
    std::println(stderr, "Failed to open {}", path);
    return EXIT_FAILURE;
  }
  auto& dev = *dev_result;

  // Enable universal planes + atomic
  if (auto r = dev.enable_universal_planes(); !r) {
    std::println(stderr, "Failed to enable universal planes");
    return EXIT_FAILURE;
  }
  if (auto r = dev.enable_atomic(); !r) {
    std::println(stderr, "Failed to enable atomic modesetting");
    return EXIT_FAILURE;
  }

  // Enumerate planes
  auto reg_result = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg_result) {
    std::println(stderr, "Failed to enumerate planes");
    return EXIT_FAILURE;
  }
  auto& registry = *reg_result;

  std::println("Found {} planes:", registry.all().size());
  for (const auto& plane : registry.all()) {
    const char* type_str = "OVERLAY";
    if (plane.type == drm::planes::DRMPlaneType::PRIMARY)
      type_str = "PRIMARY";
    else if (plane.type == drm::planes::DRMPlaneType::CURSOR)
      type_str = "CURSOR";

    std::println("  Plane {}: type={}, formats={}, crtc_mask=0x{:x}", plane.id, type_str,
                 plane.formats.size(), plane.possible_crtcs);

    if (plane.zpos_min || plane.zpos_max) {
      std::println("    zpos: [{}, {}]", plane.zpos_min.value_or(0), plane.zpos_max.value_or(0));
    }
    if (plane.supports_rotation) {
      std::println("    supports rotation");
    }
    if (plane.supports_scaling) {
      std::println("    supports scaling");
    }
  }

  // Create virtual layers
  drm::planes::Layer composition_layer;
  composition_layer.set_property("FB_ID", 1);

  drm::planes::Output output(0, composition_layer);

  // Add a primary layer (e.g., desktop background)
  auto& bg_layer = output.add_layer();
  bg_layer.set_property("FB_ID", 1)
      .set_property("CRTC_X", 0)
      .set_property("CRTC_Y", 0)
      .set_property("CRTC_W", 1920)
      .set_property("CRTC_H", 1080)
      .set_property("SRC_W", static_cast<uint64_t>(1920) << 16)
      .set_property("SRC_H", static_cast<uint64_t>(1080) << 16)
      .set_property("zpos", 0)
      .set_content_type(drm::planes::ContentType::UI);

  // Add an overlay layer (e.g., video player)
  auto& video_layer = output.add_layer();
  video_layer.set_property("FB_ID", 2)
      .set_property("CRTC_X", 100)
      .set_property("CRTC_Y", 100)
      .set_property("CRTC_W", 640)
      .set_property("CRTC_H", 480)
      .set_property("SRC_W", static_cast<uint64_t>(640) << 16)
      .set_property("SRC_H", static_cast<uint64_t>(480) << 16)
      .set_property("zpos", 1)
      .set_content_type(drm::planes::ContentType::Video)
      .set_update_hint(60);

  // Run allocator
  drm::planes::Allocator allocator(dev, registry);
  drm::AtomicRequest req(dev);

  auto result = allocator.apply(output, req, 0);
  if (!result) {
    std::println(stderr, "Allocator failed");
    return EXIT_FAILURE;
  }

  std::println("\nAllocator assigned {} layers to hardware planes", *result);

  for (const auto* layer : output.layers()) {
    if (auto plane = layer->assigned_plane_id()) {
      std::println("  Layer -> plane {}", *plane);
    } else if (layer->needs_composition()) {
      std::println("  Layer -> needs software composition");
    }
  }

  return EXIT_SUCCESS;
}
