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
// `VK_EXT_image_drm_format_modifier` for DRM/KMS interop with
// caller-supplied modifiers. The architecturally honest "Vulkan
// renders a scene layer" path is therefore:
//
//   1. Negotiate a modifier — ask `LayerScene::candidate_modifiers`
//      what KMS will accept, ask Vulkan via
//      `vkGetPhysicalDeviceImageFormatProperties2` /
//      `VkPhysicalDeviceImageDrmFormatModifierInfoEXT` what the
//      physical device can render, intersect, pick one.
//   2. Allocate a `VkImage` with that modifier and external-memory
//      handle type `DMA_BUF_BIT_EXT`.
//   3. Export the bound `VkDeviceMemory` as a DMA-BUF fd via
//      `vkGetMemoryFdKHR`.
//   4. Wrap the fd in `drm::scene::ExternalDmaBufSource` and add it
//      as a scene layer — the scene's `LayerBufferSource` interface
//      is the same as it is for any other producer.
//   5. Each frame: clear-color the image, submit + fence, scene
//      commit.
//
// What this demo does
// -------------------
//
// One Vulkan-allocated ARGB8888 image at the output's resolution,
// LINEAR modifier (universally accepted across drivers; non-LINEAR
// modifiers are valid but the negotiation dance for a portable demo
// becomes much heavier). `vkCmdClearColorImage` animates the color;
// the scene rescans out the same buffer every commit. Single-
// buffered, so there's a brief tear during the clear — acceptable
// for a demo, real applications would double-buffer.
//
// Vulkan is reached through Vulkan-Hpp's dynamic dispatcher
// (VK_NO_PROTOTYPES + a DynamicLoader that dlopen's libvulkan at
// runtime), so this links only drm-cxx — no -lvulkan — matching the
// library's vk_scanout_producer and the vk_present demo.
//
// CLI:
//
//   vulkan_scene [--seconds N] [/dev/dri/cardN]

#include "common/open_output.hpp"

#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
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

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
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

// Find the Vulkan physical device whose DRM major/minor matches the DRM fd's
// stat. Required so Vulkan-allocated DMA-BUFs can be imported by the KMS device
// that scans them out — cross-device import works on some platforms but isn't
// portable.
[[nodiscard]] vk::PhysicalDevice pick_physical_device(vk::Instance instance, int drm_fd) {
  struct stat st{};
  if (fstat(drm_fd, &st) != 0) {
    return {};
  }
  const auto want_major = static_cast<std::int64_t>(major(st.st_rdev));
  const auto want_minor = static_cast<std::int64_t>(minor(st.st_rdev));

  const std::vector<vk::PhysicalDevice> devices = instance.enumeratePhysicalDevices();
  if (devices.empty()) {
    return {};
  }
  for (vk::PhysicalDevice pd : devices) {
    const auto chain =
        pd.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDrmPropertiesEXT>();
    const auto& drm = chain.get<vk::PhysicalDeviceDrmPropertiesEXT>();
    if ((drm.hasPrimary != 0U) && drm.primaryMajor == want_major &&
        drm.primaryMinor == want_minor) {
      return pd;
    }
    if ((drm.hasRender != 0U) && drm.renderMajor == want_major && drm.renderMinor == want_minor) {
      return pd;
    }
  }
  return devices.front();  // No match — caller still gets *something*.
}

[[nodiscard]] std::uint32_t find_memory_type(vk::PhysicalDevice pd, std::uint32_t type_bits,
                                             vk::MemoryPropertyFlags flags) {
  const vk::PhysicalDeviceMemoryProperties mp = pd.getMemoryProperties();
  const drm::span<const vk::MemoryType> types{mp.memoryTypes.data(), mp.memoryTypeCount};
  for (std::uint32_t i = 0; i < types.size(); ++i) {
    if (((type_bits >> i) & 1U) == 0U) {
      continue;
    }
    if ((types[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  return 0xFFFFFFFFU;
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
    const vk::detail::DynamicLoader loader;
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));

    // Instance with the extensions we need to walk DRM properties + allocate
    // exportable DMA-BUF-backed images.
    const std::array<const char*, 2> inst_exts{
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_external_memory_capabilities",
    };
    vk::ApplicationInfo app;
    app.apiVersion = VK_API_VERSION_1_1;
    vk::InstanceCreateInfo ici;
    ici.pApplicationInfo = &app;
    ici.setPEnabledExtensionNames(inst_exts);
    const vk::Instance instance = vk::createInstance(ici);
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

    const vk::PhysicalDevice pd = pick_physical_device(instance, device.fd());
    if (!pd) {
      drm::println(stderr, "vulkan_scene: no VkPhysicalDevice matched the DRM fd");
      instance.destroy();
      return EXIT_FAILURE;
    }

    // Queue family — any with graphics capability will do; vkCmdClear doesn't
    // need a dedicated transfer queue on any current driver.
    const std::vector<vk::QueueFamilyProperties> qfs = pd.getQueueFamilyProperties();
    std::uint32_t qf_index = 0xFFFFFFFFU;
    for (std::uint32_t i = 0; i < qfs.size(); ++i) {
      if ((qfs[i].queueFlags & vk::QueueFlagBits::eGraphics)) {
        qf_index = i;
        break;
      }
    }
    if (qf_index == 0xFFFFFFFFU) {
      drm::println(stderr, "vulkan_scene: no graphics queue on the picked device");
      instance.destroy();
      return EXIT_FAILURE;
    }

    const float qp = 1.0F;
    vk::DeviceQueueCreateInfo qci;
    qci.queueFamilyIndex = qf_index;
    qci.queueCount = 1;
    qci.pQueuePriorities = &qp;

    const std::array<const char*, 4> dev_exts{
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier",
    };
    vk::DeviceCreateInfo dci;
    dci.setQueueCreateInfos(qci);
    dci.setPEnabledExtensionNames(dev_exts);
    const vk::Device vk_device = pd.createDevice(dci);
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk_device);
    const vk::Queue queue = vk_device.getQueue(qf_index, 0);

    // Allocate a VkImage with the LINEAR DRM modifier. The single-modifier list
    // is what VK_EXT_image_drm_format_modifier needs; a production renderer would
    // intersect against LayerScene::candidate_modifiers(DRM_FORMAT_ARGB8888) and
    // pick. (On rockchip VOP2, PanVK only exports LINEAR for 8888 anyway, and the
    // scene allocator lands the LINEAR buffer on an overlay plane.)
    const std::uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    const std::array<std::uint64_t, 1> modifiers{modifier};
    vk::ImageDrmFormatModifierListCreateInfoEXT drm_list;
    drm_list.setDrmFormatModifiers(modifiers);
    vk::ExternalMemoryImageCreateInfo ext_img;
    ext_img.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
    ext_img.pNext = &drm_list;

    vk::ImageCreateInfo ici_img;
    ici_img.pNext = &ext_img;
    ici_img.imageType = vk::ImageType::e2D;
    ici_img.format = vk::Format::eB8G8R8A8Unorm;  // matches DRM_FORMAT_ARGB8888 byte order
    ici_img.extent = vk::Extent3D{fb_w, fb_h, 1};
    ici_img.mipLevels = 1;
    ici_img.arrayLayers = 1;
    ici_img.samples = vk::SampleCountFlagBits::e1;
    ici_img.tiling = vk::ImageTiling::eDrmFormatModifierEXT;
    ici_img.usage = vk::ImageUsageFlagBits::eTransferDst;
    ici_img.sharingMode = vk::SharingMode::eExclusive;
    ici_img.initialLayout = vk::ImageLayout::eUndefined;
    const vk::Image vk_image = vk_device.createImage(ici_img);

    const vk::MemoryRequirements mr = vk_device.getImageMemoryRequirements(vk_image);
    vk::ExportMemoryAllocateInfo emai;
    emai.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
    vk::MemoryAllocateInfo mai;
    mai.pNext = &emai;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex =
        find_memory_type(pd, mr.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    if (mai.memoryTypeIndex == 0xFFFFFFFFU) {
      drm::println(stderr, "vulkan_scene: no DEVICE_LOCAL memory type for the exportable image");
      return EXIT_FAILURE;
    }
    const vk::DeviceMemory vk_mem = vk_device.allocateMemory(mai);
    vk_device.bindImageMemory(vk_image, vk_mem, 0);

    // Export the memory as a DMA-BUF fd.
    vk::MemoryGetFdInfoKHR mgfi;
    mgfi.memory = vk_mem;
    mgfi.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
    const int dmabuf_fd = vk_device.getMemoryFdKHR(mgfi);
    if (dmabuf_fd < 0) {
      drm::println(stderr, "vulkan_scene: vkGetMemoryFdKHR failed");
      return EXIT_FAILURE;
    }

    // Query the image's subresource layout for the row pitch + offset to feed
    // into ExternalDmaBufSource.
    vk::ImageSubresource sub;
    sub.aspectMask = vk::ImageAspectFlagBits::eMemoryPlane0EXT;
    const vk::SubresourceLayout layout = vk_device.getImageSubresourceLayout(vk_image, sub);

    drm::scene::ExternalPlaneInfo plane_info{};
    plane_info.fd = dmabuf_fd;
    plane_info.pitch = static_cast<std::uint32_t>(layout.rowPitch);
    plane_info.offset = static_cast<std::uint32_t>(layout.offset);
    const std::array<drm::scene::ExternalPlaneInfo, 1> planes{plane_info};

    auto vk_source = drm::scene::ExternalDmaBufSource::create(
        device, fb_w, fb_h, DRM_FORMAT_ARGB8888, modifier, planes);
    ::close(dmabuf_fd);  // ExternalDmaBufSource dups the fd; we close ours now.
    if (!vk_source) {
      drm::println(stderr, "ExternalDmaBufSource::create: {}", vk_source.error().message());
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

    // Command pool + buffer for the per-frame clear.
    vk::CommandPoolCreateInfo pci;
    pci.queueFamilyIndex = qf_index;
    pci.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    const vk::CommandPool cmd_pool = vk_device.createCommandPool(pci);
    vk::CommandBufferAllocateInfo cbai;
    cbai.commandPool = cmd_pool;
    cbai.level = vk::CommandBufferLevel::ePrimary;
    cbai.commandBufferCount = 1;
    const vk::CommandBuffer cmd = vk_device.allocateCommandBuffers(cbai).front();

    // First commit: brings up modeset + presents the Vulkan image before we've
    // rendered (UNDEFINED layout → zero pixels, visible briefly).
    if (auto r = scene->commit(); !r) {
      drm::println(stderr, "first commit: {}", r.error().message());
      return EXIT_FAILURE;
    }

    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    const auto deadline = t0 + std::chrono::seconds(args.seconds);
    std::uint64_t frames = 0;
    bool first = true;
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    while (clk::now() < deadline) {
      const float t = std::chrono::duration<float>(clk::now() - t0).count();
      vk::ClearColorValue clear;
      clear.float32 = std::array<float, 4>{0.5F + (0.5F * std::sin(t * 1.0F)),
                                           0.5F + (0.5F * std::sin((t * 1.3F) + 2.0F)),
                                           0.5F + (0.5F * std::sin((t * 1.7F) + 4.0F)), 1.0F};

      cmd.reset();
      vk::CommandBufferBeginInfo cbi;
      cbi.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
      cmd.begin(cbi);

      // UNDEFINED → GENERAL on the first iteration, then hold GENERAL across
      // re-clears (the right layout for the vkCmdClearColorImage path).
      vk::ImageMemoryBarrier b;
      b.oldLayout = first ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral;
      b.newLayout = vk::ImageLayout::eGeneral;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = vk_image;
      b.subresourceRange = range;
      b.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                          vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, b);

      cmd.clearColorImage(vk_image, vk::ImageLayout::eGeneral, clear, range);
      cmd.end();

      vk::SubmitInfo si;
      si.setCommandBuffers(cmd);
      queue.submit(si);
      // Single-buffered: idle the queue before letting KMS scan out. A real app
      // would chain GPU completion into the commit via IN_FENCE_FD.
      queue.waitIdle();

      if (auto r = scene->commit(); !r) {
        drm::println(stderr, "commit: {}", r.error().message());
        break;
      }
      ++frames;
      first = false;
    }
    drm::println("vulkan_scene: {} frames in {}s", frames, args.seconds);

    // Tear-down. Scene first (releases the dma-buf source's fb_id + GEM handle);
    // then Vulkan.
    scene.reset();
    vk_device.destroyCommandPool(cmd_pool);
    vk_device.freeMemory(vk_mem);
    vk_device.destroyImage(vk_image);
    vk_device.destroy();
    instance.destroy();
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
