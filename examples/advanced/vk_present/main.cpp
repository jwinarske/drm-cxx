// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// vk_present — minimal Vulkan ScanoutProducer demo.
//
// drm::present::VkScanoutProducer renders (here, an animated clear) into a
// dmabuf-exported VkImage, and drm::present::ScanoutBackend discovers the
// output, negotiates modifiers, builds the single-layer scene, and commits.
//
// The producer fully encapsulates Vulkan — libvulkan is dlopen'd at runtime by
// vulkan.hpp's dynamic dispatcher inside the library — so this demo links
// neither libvulkan nor any GL: just drm-cxx. Gated on DRM_CXX_HAS_VULKAN
// (Vulkan headers present when drm-cxx was built).
//
//   ./vk_present [/dev/dri/cardN] [frames]
//
// Run from a free VT (it holds DRM master and modesets).

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/present/scanout_backend.hpp>
#include <drm-cxx/present/vk_scanout_producer.hpp>

#include <drm_fourcc.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  const std::string dev_path = (argc > 1) ? argv[1] : "/dev/dri/card0";
  const int frames = (argc > 2) ? std::atoi(argv[2]) : 120;

  auto dev = drm::Device::open(dev_path);
  if (!dev) {
    drm::println(stderr, "vk_present: open {}: {}", dev_path, dev.error().message());
    return EXIT_FAILURE;
  }

  auto producer = drm::present::VkScanoutProducer::create(*dev);
  if (!producer) {
    drm::println(stderr, "vk_present: VkScanoutProducer::create: {} (no Vulkan device for {}?)",
                 producer.error().message(), dev_path);
    return EXIT_FAILURE;
  }

  drm::present::ScanoutBackend::Config cfg;
  cfg.fourcc = DRM_FORMAT_ARGB8888;
  auto backend = drm::present::ScanoutBackend::create(*dev, **producer, cfg);
  if (!backend) {
    drm::println(stderr, "vk_present: ScanoutBackend::create: {}", backend.error().message());
    return EXIT_FAILURE;
  }

  const auto& target = (*backend)->target();
  drm::println("vk_present: {}x{} on {} (driver {}), {} negotiated modifier(s)",
               target.mode.hdisplay, target.mode.vdisplay, dev_path, (*backend)->profile().name,
               (*backend)->modifiers().size());

  for (int frame = 0; frame < frames; ++frame) {
    // Cycle the clear color over one full period across `frames` (2*pi).
    const float phase = (static_cast<float>(frame) / static_cast<float>(frames)) * 6.2831853F;
    const std::array<float, 4> rgba{0.5F + (0.5F * std::sin(phase)), 0.3F,
                                    0.5F + (0.5F * std::cos(phase)), 1.0F};
    if (auto r = (*producer)->render_clear(rgba); !r) {
      drm::println(stderr, "vk_present: render_clear: {}", r.error().message());
      return EXIT_FAILURE;
    }
    if (auto r = (*backend)->present(0); !r) {
      drm::println(stderr, "vk_present: present: {}", r.error().message());
      return EXIT_FAILURE;
    }
  }

  drm::println("vk_present: presented {} frames", frames);
  return EXIT_SUCCESS;
}
