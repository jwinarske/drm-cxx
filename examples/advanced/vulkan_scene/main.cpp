// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// vulkan_scene — minimal end-to-end demo of a Vulkan-rendered layer in
// a `drm::scene::LayerScene`.
//
// Why this doesn't use `GbmSurfaceSource`
// --------------------------------------
//
// `gbm_surface` is an EGL-only concept (Mesa's `EGL_KHR_platform_gbm`
// fronts it). Vulkan has no `VK_EXT_gbm_surface` extension — the
// closest API surface is `VK_KHR_display` (direct-to-CRTC, what the
// existing `vulkan_display` example uses) plus
// `VK_EXT_image_drm_format_modifier` for DRM/KMS interop. A Vulkan
// renderer feeding a scene layer goes through
// `drm::present::VkScanoutProducer` instead:
//
//   1. Negotiate a modifier — `LayerScene::candidate_modifiers` (what KMS
//      will accept) intersected with `VkScanoutProducer::exportable_modifiers`
//      (what the device can render).
//   2. `create_buffer` allocates the scanout images and returns the scene
//      layer's `LayerBufferSource`. Where the display can import Vulkan's
//      memory that is zero-copy over a rotating set of images; where it can't
//      (a display controller without an IOMMU, e.g. i.MX8M Plus) each frame is
//      copied on the GPU or the CPU — the renderer does not change.
//   3. Each frame: `render()` hands a command buffer and the frame's image to
//      the renderer (here a clear-color), then the scene commits.
//
// The renderer records against the producer's own VkDevice, reached through
// Vulkan-Hpp's dynamic dispatcher (VK_NO_PROTOTYPES + a DynamicLoader that
// dlopen's libvulkan at runtime), so this links only drm-cxx — no -lvulkan.
//
// CLI:
//
//   vulkan_scene [--seconds N] [/dev/dri/cardN]

#include "common/open_output.hpp"

#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/present/vk_scanout_producer.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) -- vulkan.hpp config knobs; with
// VK_NO_PROTOTYPES no C entry points are referenced, so nothing links libvulkan.
#define VK_NO_PROTOTYPES
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <drm_fourcc.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(misc-include-cleaner) -- storage for the default dispatcher.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace {

constexpr int k_default_seconds = 5;

struct Args {
  int seconds{k_default_seconds};
};

[[nodiscard]] Args parse_args(int& argc, char**& argv) {
  Args a;
  int write = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--seconds" && (i + 1) < argc) {
      a.seconds = std::atoi(argv[++i]);
    } else {
      argv[write++] = argv[i];
    }
  }
  argc = write;
  return a;
}

}  // namespace

int main(int argc, char* argv[]) try {
  const auto args = parse_args(argc, argv);

  auto out = drm::examples::open_and_pick_output(argc, argv);
  if (!out) {
    return EXIT_FAILURE;
  }
  auto& device = out->device;
  const std::uint32_t fb_w = out->mode.hdisplay;
  const std::uint32_t fb_h = out->mode.vdisplay;
  drm::println("vulkan_scene: crtc={} connector={} mode={}x{}@{}Hz", out->crtc_id,
               out->connector_id, fb_w, fb_h, out->mode.vrefresh);

  // The producer before the scene: it must outlive the scene it feeds (the
  // scene's layer source points into it), so it is destroyed after.
  auto producer_r = drm::present::VkScanoutProducer::create(device);
  if (!producer_r) {
    drm::println(stderr, "VkScanoutProducer::create: {}", producer_r.error().message());
    return EXIT_FAILURE;
  }
  auto& producer = *producer_r;

  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = out->crtc_id;
  scene_cfg.connector_id = out->connector_id;
  scene_cfg.mode = out->mode;
  auto scene_r = drm::scene::LayerScene::create(device, scene_cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto& scene = *scene_r;

  // Background dumb-buffer layer keeps PRIMARY armed across modeset (same dance
  // the EGL demo does).
  auto bg_source = drm::scene::DumbBufferSource::create(device, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  if (!bg_source) {
    drm::println(stderr, "DumbBufferSource::create: {}", bg_source.error().message());
    return EXIT_FAILURE;
  }
  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_source);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.zpos = 2;
  if (auto r = scene->add_layer(std::move(bg_desc)); !r) {
    drm::println(stderr, "add_layer (background): {}", r.error().message());
    return EXIT_FAILURE;
  }

  try {
    // Renderer side: this binary's Vulkan-Hpp dispatcher, pointed at the
    // producer's instance and device.
    const vk::detail::DynamicLoader loader;
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
        vk::Instance(static_cast<VkInstance>(producer->vk_instance())));
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(static_cast<VkDevice>(producer->vk_device())));

    // Modifiers both sides accept; LINEAR when they share none.
    std::vector<std::uint64_t> modifiers;
    const auto renderable = producer->exportable_modifiers(DRM_FORMAT_ARGB8888);
    for (const std::uint64_t m : scene->candidate_modifiers(DRM_FORMAT_ARGB8888)) {
      if (std::find(renderable.begin(), renderable.end(), m) != renderable.end()) {
        modifiers.push_back(m);
      }
    }
    if (modifiers.empty()) {
      modifiers.push_back(DRM_FORMAT_MOD_LINEAR);
    }
    auto vk_source =
        producer->create_buffer(fb_w, fb_h, DRM_FORMAT_ARGB8888,
                                drm::span<const std::uint64_t>(modifiers.data(), modifiers.size()));
    if (!vk_source) {
      drm::println(stderr, "VkScanoutProducer::create_buffer: {}", vk_source.error().message());
      return EXIT_FAILURE;
    }

    drm::scene::LayerDesc fg_desc;
    fg_desc.source = std::move(*vk_source);
    fg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
    fg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
    fg_desc.display.zpos = 3;
    if (auto r = scene->add_layer(std::move(fg_desc)); !r) {
      drm::println(stderr, "add_layer (vulkan): {}", r.error().message());
      return EXIT_FAILURE;
    }

    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    const auto deadline = t0 + std::chrono::seconds(args.seconds);
    std::uint64_t frames = 0;
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    while (clk::now() < deadline) {
      const float t = std::chrono::duration<float>(clk::now() - t0).count();
      vk::ClearColorValue clear;
      clear.float32 = std::array<float, 4>{0.5F + (0.5F * std::sin(t * 1.0F)),
                                           0.5F + (0.5F * std::sin((t * 1.3F) + 2.0F)),
                                           0.5F + (0.5F * std::sin((t * 1.7F) + 4.0F)), 1.0F};

      // The image arrives in GENERAL and must be left there; the producer owns
      // the barriers and the hand-off to the display.
      auto r = producer->render([&](void* cmd, void* image) {
        vk::CommandBuffer(static_cast<VkCommandBuffer>(cmd))
            .clearColorImage(vk::Image(static_cast<VkImage>(image)), vk::ImageLayout::eGeneral,
                             clear, range);
      });
      if (!r) {
        drm::println(stderr, "render: {}", r.error().message());
        break;
      }
      if (auto c = scene->commit(); !c) {
        drm::println(stderr, "commit: {}", c.error().message());
        break;
      }
      ++frames;
    }
    drm::println("vulkan_scene: {} frames in {}s", frames, args.seconds);

    // The scene releases its layers (and their fb_ids) before the producer
    // frees the memory behind them.
    scene.reset();
  } catch (const std::exception& e) {
    drm::println(stderr, "vulkan_scene: vulkan error: {}", e.what());
    return EXIT_FAILURE;
  } catch (...) {
    drm::println(stderr, "vulkan_scene: unknown vulkan error");
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
} catch (...) {
  return EXIT_FAILURE;
}
