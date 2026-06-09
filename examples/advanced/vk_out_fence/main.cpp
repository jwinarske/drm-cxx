// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// vk_out_fence — validate OUT_FENCE (the scanout-completion sync_file), the
// counterpart to vk_present's IN_FENCE acquire-fence demo.
//
// Each frame: render via the Vulkan producer, present requesting an OUT_FENCE,
// then WAIT on it. A signaled OUT_FENCE means the frame reached scanout — so
// waiting on it is the display-done reuse gate that makes single-buffer
// rendering tear-safe (don't overwrite the buffer until the display is done).
//
// Links only drm-cxx (the producer encapsulates Vulkan). Gated on
// DRM_CXX_HAS_VULKAN. Run from a free VT (it holds DRM master and modesets).
//
//   ./vk_out_fence [/dev/dri/cardN] [frames]

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/present/scanout_backend.hpp>
#include <drm-cxx/present/vk_scanout_producer.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <drm_fourcc.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  const std::string dev_path = (argc > 1) ? argv[1] : "/dev/dri/card0";
  const int frames = (argc > 2) ? std::atoi(argv[2]) : 60;

  auto dev = drm::Device::open(dev_path);
  if (!dev) {
    drm::println(stderr, "vk_out_fence: open {}: {}", dev_path, dev.error().message());
    return EXIT_FAILURE;
  }
  auto producer = drm::present::VkScanoutProducer::create(*dev);
  if (!producer) {
    drm::println(stderr, "vk_out_fence: VkScanoutProducer::create: {}", producer.error().message());
    return EXIT_FAILURE;
  }
  drm::present::ScanoutBackend::Config cfg;
  cfg.fourcc = DRM_FORMAT_ARGB8888;
  auto backend = drm::present::ScanoutBackend::create(*dev, **producer, cfg);
  if (!backend) {
    drm::println(stderr, "vk_out_fence: ScanoutBackend::create: {}", backend.error().message());
    return EXIT_FAILURE;
  }
  const auto& target = (*backend)->target();
  drm::println("vk_out_fence: {}x{} on {} (driver {})", target.mode.hdisplay, target.mode.vdisplay,
               dev_path, (*backend)->profile().name);

  int produced = 0;
  int signaled = 0;
  for (int frame = 0; frame < frames; ++frame) {
    const float phase = (static_cast<float>(frame) / static_cast<float>(frames)) * 6.2831853F;
    const std::array<float, 4> rgba{0.5F + (0.5F * std::sin(phase)), 0.3F,
                                    0.5F + (0.5F * std::cos(phase)), 1.0F};
    if (auto r = (*producer)->render_clear(rgba); !r) {
      drm::println(stderr, "vk_out_fence: render_clear: {}", r.error().message());
      return EXIT_FAILURE;
    }

    drm::sync::SyncFence out_fence;
    auto r = (*backend)->present(0, &out_fence);
    if (!r) {
      drm::println(stderr, "vk_out_fence: present: {}", r.error().message());
      return EXIT_FAILURE;
    }
    if (out_fence.valid()) {
      ++produced;
      // The display-done reuse gate: wait for scanout before the next render
      // overwrites the buffer.
      if (out_fence.wait(std::chrono::seconds(1))) {
        ++signaled;
      } else {
        drm::println(stderr, "vk_out_fence: frame {} OUT_FENCE wait timed out", frame);
      }
    } else if (frame == 0) {
      drm::println("vk_out_fence: OUT_FENCE not produced (driver lacks OUT_FENCE_PTR)");
    }
  }

  drm::println("vk_out_fence: presented {} frames; OUT_FENCE produced on {}, signaled on {}",
               frames, produced, signaled);
  return EXIT_SUCCESS;
}
