// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// examples/advanced/vulkan_offload_scanout/main.cpp
//
// Vulkan counterpart to egl_offload_scanout: render on a Vulkan device into a
// VK_EXT_image_drm_format_modifier image whose modifier is chosen from the
// intersection of (a) what the device can render and (b) what the display plane
// can scan out, export the backing memory as a dma-buf, import it on the display
// node, and let the TEST_ONLY commit decide. The display's FormatTable is the
// same ground truth the EGL path uses; only the producer changed.
//
// Vulkan is reached through Vulkan-Hpp's dynamic dispatcher (VK_NO_PROTOTYPES +
// a DynamicLoader that dlopen's libvulkan at runtime), so this example links
// only drm-cxx — no -lvulkan — matching src/present/vk_scanout_producer.cpp and
// the vk_present / vulkan_display demos.
//
// Run:  ./vulkan_offload_scanout [display=/dev/dri/card0]

#include "../../common/kms_present.hpp"

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) -- vulkan.hpp config knobs; with
// VK_NO_PROTOTYPES no C entry points are referenced, so nothing links libvulkan.
#define VK_NO_PROTOTYPES
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(misc-include-cleaner) -- storage for the default dispatcher.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace fmt = drm::fmt;

int main(int argc, char** argv) {
  const char* disp_path = argc > 1 ? argv[1] : "/dev/dri/card0";

  // --- display node + its scanout capabilities ----------------------------
  int const disp_fd = open(disp_path, O_RDWR | O_CLOEXEC);
  if (disp_fd < 0) {
    std::perror("open display");
    return 1;
  }
  drmSetClientCap(disp_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCap(disp_fd, DRM_CLIENT_CAP_ATOMIC, 1);

  auto target = kms::pick_target(disp_fd);
  if (!target) {
    std::fprintf(stderr, "no connected output\n");
    return 1;
  }
  const std::uint32_t w = target->mode.hdisplay;
  const std::uint32_t h = target->mode.vdisplay;

  auto disp_tbl = fmt::FormatTable::from_plane(disp_fd, target->primary_plane);
  if (!disp_tbl) {
    std::fprintf(stderr, "display plane has no IN_FORMATS: %s\n",
                 disp_tbl.error().message().c_str());
    return 1;
  }
  std::printf("display %s: crtc %u, plane %u, %ux%u\n", disp_path, target->crtc_id,
              target->primary_plane, w, h);

  try {
    // --- Vulkan loader + instance + device with dma-buf export --------------
    const vk::detail::DynamicLoader loader;
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));

    vk::ApplicationInfo app;
    app.apiVersion = VK_API_VERSION_1_1;
    vk::InstanceCreateInfo ici;
    ici.pApplicationInfo = &app;
    const vk::Instance inst = vk::createInstance(ici);
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(inst);

    const std::vector<vk::PhysicalDevice> gpus = inst.enumeratePhysicalDevices();
    if (gpus.empty()) {
      std::fprintf(stderr, "no Vulkan device\n");
      return 1;
    }
    // [0] for brevity; match VK_EXT_physical_device_drm to the display node on
    // multi-GPU systems.
    const vk::PhysicalDevice gpu = gpus[0];

    const std::vector<vk::QueueFamilyProperties> qf = gpu.getQueueFamilyProperties();
    std::uint32_t qfi = 0;
    for (std::uint32_t i = 0; i < qf.size(); ++i) {
      if ((qf[i].queueFlags & vk::QueueFlagBits::eGraphics)) {
        qfi = i;
        break;
      }
    }

    const float prio = 1.0F;
    vk::DeviceQueueCreateInfo qci;
    qci.queueFamilyIndex = qfi;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const std::array<const char*, 5> dev_exts{
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    vk::DeviceCreateInfo dci;
    dci.setQueueCreateInfos(qci);
    dci.setPEnabledExtensionNames(dev_exts);
    const vk::Device dev = gpu.createDevice(dci);
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(dev);
    const vk::Queue queue = dev.getQueue(qfi, 0);

    // --- pick a (format, modifier) the GPU can render AND the display can scan
    // out. Channel order matters per driver: e.g. PanVK exports only LINEAR for
    // 8888 RGBA/BGRA, while rockchip VOP2 takes ARGB/XRGB only as AFBC but
    // ABGR/XBGR as LINEAR -- so on that stack the RGBA-order pairs are the ones
    // that actually intersect. Try BGRA first (amdgpu's native), then RGBA.
    struct FmtPair {
      vk::Format vkf;
      std::uint32_t fourcc;
    };
    const std::array<FmtPair, 4> pairs{{
        {vk::Format::eB8G8R8A8Unorm, DRM_FORMAT_ARGB8888},
        {vk::Format::eB8G8R8A8Unorm, DRM_FORMAT_XRGB8888},
        {vk::Format::eR8G8B8A8Unorm, DRM_FORMAT_ABGR8888},
        {vk::Format::eR8G8B8A8Unorm, DRM_FORMAT_XBGR8888},
    }};
    const vk::FormatFeatureFlags need =
        vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eTransferDst;
    const bool dump_mods = std::getenv("DRM_FMT_DUMP_VK_MODS") != nullptr;

    vk::Format vk_format{};
    std::uint32_t kms_fourcc = 0;
    std::vector<fmt::Modifier> candidates;
    std::vector<vk::DrmFormatModifierPropertiesEXT> mod_props;
    for (const auto& pr : pairs) {
      vk::DrmFormatModifierPropertiesListEXT mod_list;
      vk::FormatProperties2 fp;
      fp.pNext = &mod_list;
      gpu.getFormatProperties2(pr.vkf, &fp);
      std::vector<vk::DrmFormatModifierPropertiesEXT> props(mod_list.drmFormatModifierCount);
      mod_list.pDrmFormatModifierProperties = props.data();
      gpu.getFormatProperties2(pr.vkf, &fp);

      if (dump_mods) {
        std::printf("  VK[%c%c%c%c]: %zu modifier(s) reported by the GPU\n", char(pr.fourcc),
                    char(pr.fourcc >> 8), char(pr.fourcc >> 16), char(pr.fourcc >> 24),
                    props.size());
      }
      std::vector<fmt::Modifier> cands;
      for (const auto& mp : props) {
        const fmt::Modifier m{mp.drmFormatModifier};
        const bool renderable = (mp.drmFormatModifierTilingFeatures & need) == need;
        const bool scannable = disp_tbl->supports(pr.fourcc, m);
        if (dump_mods) {
          std::printf("  VK[%c%c%c%c] %-32s render=%d display=%d\n", char(pr.fourcc),
                      char(pr.fourcc >> 8), char(pr.fourcc >> 16), char(pr.fourcc >> 24),
                      fmt::describe(m).c_str(), renderable ? 1 : 0, scannable ? 1 : 0);
        }
        if (renderable && scannable) {
          cands.push_back(m);
        }
      }
      if (!cands.empty()) {
        vk_format = pr.vkf;
        kms_fourcc = pr.fourcc;
        candidates = std::move(cands);
        mod_props = std::move(props);
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
      cand_vals.push_back(m.value);
    }

    // --- create the modifier image, exportable -----------------------------
    vk::ImageDrmFormatModifierListCreateInfoEXT mod_ci;
    mod_ci.setDrmFormatModifiers(cand_vals);
    vk::ExternalMemoryImageCreateInfo ext_ci;
    ext_ci.pNext = &mod_ci;
    ext_ci.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

    vk::ImageCreateInfo img_ci;
    img_ci.pNext = &ext_ci;
    img_ci.imageType = vk::ImageType::e2D;
    img_ci.format = vk_format;
    img_ci.extent = vk::Extent3D{w, h, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = vk::SampleCountFlagBits::e1;
    img_ci.tiling = vk::ImageTiling::eDrmFormatModifierEXT;
    img_ci.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
    img_ci.sharingMode = vk::SharingMode::eExclusive;
    img_ci.initialLayout = vk::ImageLayout::eUndefined;
    const vk::Image image = dev.createImage(img_ci);

    const vk::MemoryRequirements mr = dev.getImageMemoryRequirements(image);
    const vk::PhysicalDeviceMemoryProperties mp = gpu.getMemoryProperties();
    const drm::span<const vk::MemoryType> mem_types{mp.memoryTypes.data(), mp.memoryTypeCount};
    std::uint32_t mem_type = 0;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      if (((mr.memoryTypeBits & (1U << i)) != 0U) &&
          (mem_types[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
        mem_type = i;
        break;
      }
    }

    vk::ExportMemoryAllocateInfo exp;
    exp.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
    vk::MemoryDedicatedAllocateInfo ded;
    ded.pNext = &exp;
    ded.image = image;
    vk::MemoryAllocateInfo mai;
    mai.pNext = &ded;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type;
    const vk::DeviceMemory mem = dev.allocateMemory(mai);
    dev.bindImageMemory(image, mem, 0);

    // chosen modifier + per-plane layout
    const vk::ImageDrmFormatModifierPropertiesEXT chosen_props =
        dev.getImageDrmFormatModifierPropertiesEXT(image);
    const fmt::Modifier chosen{chosen_props.drmFormatModifier};
    std::printf("GPU rendered into: %s\n", fmt::describe(chosen).c_str());

    unsigned nplanes = 1;
    for (const auto& mp2 : mod_props) {
      if (mp2.drmFormatModifier == chosen_props.drmFormatModifier) {
        nplanes = mp2.drmFormatModifierPlaneCount;
        break;
      }
    }

    std::vector<fmt::ScanoutBuffer::ImportDesc::Plane> planes(nplanes);
    for (unsigned i = 0; i < nplanes; ++i) {
      vk::ImageSubresource sub;
      sub.aspectMask =
          static_cast<vk::ImageAspectFlagBits>(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i);
      const vk::SubresourceLayout sl = dev.getImageSubresourceLayout(image, sub);
      planes[i].stride = static_cast<std::uint32_t>(sl.rowPitch);
      planes[i].offset = static_cast<std::uint32_t>(sl.offset);
    }

    // --- record: clear, then release to the foreign (display) queue --------
    vk::CommandPoolCreateInfo cpci;
    cpci.queueFamilyIndex = qfi;
    const vk::CommandPool pool = dev.createCommandPool(cpci);
    vk::CommandBufferAllocateInfo cbai;
    cbai.commandPool = pool;
    cbai.level = vk::CommandBufferLevel::ePrimary;
    cbai.commandBufferCount = 1;
    const vk::CommandBuffer cmd = dev.allocateCommandBuffers(cbai).front();

    vk::CommandBufferBeginInfo bi;
    bi.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    cmd.begin(bi);

    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::ImageMemoryBarrier to_general;
    to_general.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    to_general.oldLayout = vk::ImageLayout::eUndefined;
    to_general.newLayout = vk::ImageLayout::eGeneral;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = image;
    to_general.subresourceRange = range;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
                        {}, {}, {}, to_general);

    vk::ClearColorValue color;
    color.float32 = std::array<float, 4>{0.20F, 0.55F, 0.85F, 1.0F};
    cmd.clearColorImage(image, vk::ImageLayout::eGeneral, color, range);

    // Release ownership to the foreign queue: the display engine reads it
    // outside Vulkan's queue model.
    vk::ImageMemoryBarrier release;
    release.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    release.oldLayout = vk::ImageLayout::eGeneral;
    release.newLayout = vk::ImageLayout::eGeneral;
    release.srcQueueFamilyIndex = qfi;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    release.image = image;
    release.subresourceRange = range;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, release);
    cmd.end();

    vk::SubmitInfo si;
    si.setCommandBuffers(cmd);
    queue.submit(si);
    queue.waitIdle();  // retire before the display reads it

    // --- export the backing memory once; all planes share the fd at offsets -
    vk::MemoryGetFdInfoKHR gfi;
    gfi.memory = mem;
    gfi.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
    int const dmabuf = dev.getMemoryFdKHR(gfi);
    for (unsigned i = 0; i < nplanes; ++i) {
      planes[i].dmabuf_fd = dmabuf;  // same fd
    }

    fmt::ScanoutBuffer::ImportDesc desc;  // explicit init (no designated inits)
    desc.width = w;
    desc.height = h;
    desc.fourcc = kms_fourcc;
    desc.modifier = chosen;
    desc.planes = planes;  // std::vector -> drm::span

    auto fb = fmt::ScanoutBuffer::import_dmabuf(disp_fd, desc);
    close(dmabuf);  // import took its own ref(s)
    if (!fb) {
      std::fprintf(stderr, "import_dmabuf on display node: %s\n", fb.error().message().c_str());
      return 1;
    }

    // --- ground truth + present --------------------------------------------
    int const test = kms::commit_fb(disp_fd, *target, fb->fb_id(),
                                    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET);
    std::printf("display TEST_ONLY of %s: %s\n", fmt::describe(chosen).c_str(),
                test == 0 ? "ACCEPTED" : "REJECTED");
    if (test != 0) {
      std::fprintf(stderr,
                   "the display node rejected the GPU's modifier -- drop the edge "
                   "and renegotiate toward LINEAR.\n");
      return 1;
    }
    if (int const r =
            kms::commit_fb(disp_fd, *target, fb->fb_id(), DRM_MODE_ATOMIC_ALLOW_MODESET)) {
      std::fprintf(stderr, "present: %s\n", std::strerror(-r));
      return 1;
    }
    std::printf("on screen (Vulkan-rendered %s scanned out by the display) -- 3s\n",
                fmt::describe(chosen).c_str());
    sleep(3);

    dev.destroyCommandPool(pool);
    dev.destroyImage(image);
    dev.freeMemory(mem);
    dev.destroy();
    inst.destroy();
    // `fb` borrows disp_fd for its RmFB on teardown, so release it before the
    // fd is closed rather than at function-scope end.
    {
      const fmt::ScanoutBuffer released = std::move(*fb);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "vulkan: %s\n", e.what());
    close(disp_fd);
    return 1;
  } catch (...) {
    std::fprintf(stderr, "vulkan: unknown error\n");
    close(disp_fd);
    return 1;
  }

  close(disp_fd);
  return 0;
}
