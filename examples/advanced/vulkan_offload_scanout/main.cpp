// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// examples/advanced/vulkan_offload_scanout/main.cpp
//
// Vulkan counterpart to egl_offload_scanout: render on a Vulkan device into an
// image whose DRM format modifier is chosen from the intersection of (a) what
// the device can render and (b) what the display plane can scan out, ranked by
// bandwidth (compression, then tiling, then LINEAR), and scan it out. The
// display's FormatTable is the same ground truth the EGL path uses; only the
// producer changed.
//
// The ranked candidates go to drm::present::VkScanoutProducer, which allocates
// with the best one the driver takes and hands the scene a layer source; the
// scene's TEST_ONLY commit has the final word. Where the display cannot import
// Vulkan's memory at all (a display controller without an IOMMU, e.g. i.MX8M
// Plus) the producer falls back to copying each frame — on the GPU or the CPU —
// into a display-side buffer, and the line printed at the end says which.
//
// Vulkan is reached through Vulkan-Hpp's dynamic dispatcher (VK_NO_PROTOTYPES +
// a DynamicLoader that dlopen's libvulkan at runtime), so this example links
// only drm-cxx — no -lvulkan.
//
// Run:  ./vulkan_offload_scanout [display=/dev/dri/card0]
//   DRM_FMT_DUMP_VK_MODS=1      list every modifier the GPU reports
//   DRM_FMT_FORCE_COMPRESSION=1 offer only compression modifiers

#include "../../common/kms_present.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/present/vk_scanout_producer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) -- vulkan.hpp config knobs; with
// VK_NO_PROTOTYPES no C entry points are referenced, so nothing links libvulkan.
#define VK_NO_PROTOTYPES
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <drm.h>
#include <drm_fourcc.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <xf86drm.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <unistd.h>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(misc-include-cleaner) -- storage for the default dispatcher.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace fmt = drm::fmt;

int main(int argc, char** argv) try {
  const char* disp_path = argc > 1 ? argv[1] : "/dev/dri/card0";

  // --- display node + its scanout capabilities ----------------------------
  auto dev = drm::Device::open(disp_path);
  if (!dev) {
    std::fprintf(stderr, "open %s: %s\n", disp_path, dev.error().message().c_str());
    return 1;
  }
  const int disp_fd = dev->fd();
  drmSetClientCap(disp_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCap(disp_fd, DRM_CLIENT_CAP_ATOMIC, 1);

  auto target = kms::pick_target(disp_fd);
  if (!target) {
    std::fprintf(stderr, "no connected output\n");
    return 1;
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;

  const auto disp_tbl = kms::plane_format_table(disp_fd, target->primary_plane);
  std::printf("display %s: crtc %u, plane %u, %ux%u\n", disp_path, target->crtc_id,
              target->primary_plane, w, h);

  // --- the Vulkan producer (device matched to this display node) ----------
  auto producer_r = drm::present::VkScanoutProducer::create(*dev);
  if (!producer_r) {
    std::fprintf(stderr, "VkScanoutProducer::create: %s\n", producer_r.error().message().c_str());
    return 1;
  }
  auto& producer = *producer_r;

  // --- pick a (format, modifier) the GPU can render AND the display can scan
  // out. Channel order matters per driver: e.g. PanVK exports only LINEAR for
  // 8888 RGBA/BGRA, while rockchip VOP2 takes ARGB/XRGB only as AFBC but
  // ABGR/XBGR as LINEAR -- so on that stack the RGBA-order pairs are the ones
  // that actually intersect. Try BGRA first (amdgpu's native), then RGBA.
  const std::array<std::uint32_t, 4> fourccs{DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
                                             DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888};
  const bool dump_mods = std::getenv("DRM_FMT_DUMP_VK_MODS") != nullptr;

  std::uint32_t kms_fourcc = 0;
  std::vector<fmt::Modifier> candidates;
  for (const std::uint32_t fourcc : fourccs) {
    const std::vector<std::uint64_t> renderable = producer->exportable_modifiers(fourcc);
    if (dump_mods) {
      std::printf("  VK[%c%c%c%c]: %zu renderable modifier(s)\n", char(fourcc), char(fourcc >> 8),
                  char(fourcc >> 16), char(fourcc >> 24), renderable.size());
    }
    std::vector<fmt::Modifier> cands;
    for (const std::uint64_t value : renderable) {
      const fmt::Modifier m{value};
      const bool scannable = disp_tbl.supports(fourcc, m);
      if (dump_mods) {
        std::printf("  VK[%c%c%c%c] %-32s display=%d\n", char(fourcc), char(fourcc >> 8),
                    char(fourcc >> 16), char(fourcc >> 24), fmt::describe(m).c_str(),
                    scannable ? 1 : 0);
      }
      if (scannable) {
        cands.push_back(m);
      }
    }
    if (!cands.empty()) {
      kms_fourcc = fourcc;
      candidates = std::move(cands);
      break;
    }
  }
  if (candidates.empty()) {
    std::fprintf(stderr,
                 "no modifier both the GPU can render and the display can scan "
                 "out for any candidate format\n");
    return 1;
  }
  std::printf("using fourcc '%c%c%c%c'\n", char(kms_fourcc), char(kms_fourcc >> 8),
              char(kms_fourcc >> 16), char(kms_fourcc >> 24));
  // COMPRESSION first, then tiling, LINEAR last (stable to keep VK's order
  // within a class).
  std::stable_sort(candidates.begin(), candidates.end(), [](fmt::Modifier a, fmt::Modifier b) {
    auto rank = [](fmt::Modifier m) {
      switch (fmt::classify(m)) {
        case fmt::BandwidthClass::Compression:
          return 0;
        case fmt::BandwidthClass::Tiling:
          return 1;
        case fmt::BandwidthClass::Linear:
          return 2;
      }
      return 3;
    };
    return rank(a) < rank(b);
  });

  // Diagnostic: force a compression-only list, to prove a real compressed
  // buffer can be both rendered and scanned out. Set DRM_FMT_FORCE_COMPRESSION=1.
  if (std::getenv("DRM_FMT_FORCE_COMPRESSION") != nullptr) {
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [](fmt::Modifier m) {
                                      return fmt::classify(m) != fmt::BandwidthClass::Compression;
                                    }),
                     candidates.end());
    if (candidates.empty()) {
      std::fprintf(stderr,
                   "DRM_FMT_FORCE_COMPRESSION set but no compression modifier is both "
                   "GPU-renderable and display-scannable for this format\n");
      return 1;
    }
  }
  std::vector<std::uint64_t> cand_vals;
  cand_vals.reserve(candidates.size());
  for (fmt::Modifier const m : candidates) {
    std::printf("  candidate: %s\n", fmt::describe(m).c_str());
    cand_vals.push_back(m.value);
  }

  // --- scene with the producer's buffer as its only layer ------------------
  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = target->crtc_id;
  scene_cfg.connector_id = target->connector_id;
  scene_cfg.mode = target->mode;
  auto scene = drm::scene::LayerScene::create(*dev, scene_cfg);
  if (!scene) {
    std::fprintf(stderr, "LayerScene::create: %s\n", scene.error().message().c_str());
    return 1;
  }
  auto source = producer->create_buffer(
      w, h, kms_fourcc, drm::span<const std::uint64_t>(cand_vals.data(), cand_vals.size()));
  if (!source) {
    std::fprintf(stderr, "VkScanoutProducer::create_buffer: %s\n",
                 source.error().message().c_str());
    return 1;
  }
  const fmt::Modifier chosen{(*source)->format().modifier};
  drm::scene::LayerDesc desc;
  desc.source = std::move(*source);
  desc.display.src_rect = drm::scene::Rect{0, 0, w, h};
  desc.display.dst_rect = drm::scene::Rect{0, 0, w, h};
  if (auto r = (*scene)->add_layer(std::move(desc)); !r) {
    std::fprintf(stderr, "add_layer: %s\n", r.error().message().c_str());
    return 1;
  }

  // --- render: one clear, recorded against the producer's device ----------
  const vk::detail::DynamicLoader loader;
  // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
  VULKAN_HPP_DEFAULT_DISPATCHER.init(
      loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
  // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
  VULKAN_HPP_DEFAULT_DISPATCHER.init(
      vk::Instance(static_cast<VkInstance>(producer->vk_instance())));
  // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(static_cast<VkDevice>(producer->vk_device())));

  const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
  vk::ClearColorValue color;
  color.float32 = std::array<float, 4>{0.20F, 0.55F, 0.85F, 1.0F};
  if (auto r = producer->render([&](void* cmd, void* image) {
        vk::CommandBuffer(static_cast<VkCommandBuffer>(cmd))
            .clearColorImage(vk::Image(static_cast<VkImage>(image)), vk::ImageLayout::eGeneral,
                             color, range);
      });
      !r) {
    std::fprintf(stderr, "render: %s\n", r.error().message().c_str());
    return 1;
  }

  // --- ground truth: the scene TEST_ONLYs its plane choice, then commits ---
  auto report = (*scene)->commit();
  if (!report) {
    std::fprintf(stderr, "the display rejected the frame (%s) -- renegotiate toward LINEAR.\n",
                 report.error().message().c_str());
    return 1;
  }
  std::printf("on screen: Vulkan-rendered '%c%c%c%c' %s (%s) -- 3s\n", char(kms_fourcc),
              char(kms_fourcc >> 8), char(kms_fourcc >> 16), char(kms_fourcc >> 24),
              fmt::describe(chosen).c_str(),
              report->layers_composited > 0 ? "composited" : "on a plane");
  sleep(3);

  scene->reset();  // the scene releases the layer before the producer frees its memory
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "vulkan_offload_scanout: %s\n", e.what());
  return 1;
} catch (...) {
  std::fprintf(stderr, "vulkan_offload_scanout: unknown error\n");
  return 1;
}
