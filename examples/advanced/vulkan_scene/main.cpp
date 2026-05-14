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
// CLI:
//
//   vulkan_scene [--seconds N] [/dev/dri/cardN]

#include "common/open_output.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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

// Find the Vulkan physical device whose DRM major/minor matches the
// DRM fd's stat. Required so Vulkan-allocated DMA-BUFs can be
// imported by the KMS device that scans them out — cross-device
// import works on some platforms but isn't portable.
[[nodiscard]] VkPhysicalDevice pick_physical_device(VkInstance instance, int drm_fd) {
  struct stat st {};
  if (fstat(drm_fd, &st) != 0) {
    return VK_NULL_HANDLE;
  }
  const unsigned int want_major = major(st.st_rdev);
  const unsigned int want_minor = minor(st.st_rdev);

  std::uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (count == 0) {
    return VK_NULL_HANDLE;
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());

  auto get_drm_props =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
  if (get_drm_props == nullptr) {
    return devices.front();  // No properties2 — fall back to first.
  }

  for (auto pd : devices) {
    VkPhysicalDeviceDrmPropertiesEXT drm_props{};
    drm_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &drm_props;
    get_drm_props(pd, &props2);

    if (drm_props.hasPrimary != VK_FALSE && drm_props.primaryMajor == want_major &&
        drm_props.primaryMinor == want_minor) {
      return pd;
    }
    if (drm_props.hasRender != VK_FALSE && drm_props.renderMajor == want_major &&
        drm_props.renderMinor == want_minor) {
      return pd;
    }
  }
  return devices.front();  // No match — caller still gets *something*.
}

[[nodiscard]] std::uint32_t find_memory_type(VkPhysicalDevice pd, std::uint32_t type_bits,
                                             VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (((type_bits >> i) & 1U) != 0U &&
        (mp.memoryTypes[i].propertyFlags & flags) == flags) {  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      return i;
    }
  }
  return 0xFFFFFFFFU;
}

}  // namespace

int main(int argc, char* argv[]) {
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

  // Background dumb-buffer layer keeps PRIMARY armed across modeset
  // (same dance the EGL demo does).
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

  // Vulkan instance with the extensions we need to walk DRM
  // properties + allocate exportable DMA-BUF-backed images.
  const std::array<const char*, 2> inst_exts{
      "VK_KHR_get_physical_device_properties2",
      "VK_KHR_external_memory_capabilities",
  };
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = static_cast<std::uint32_t>(inst_exts.size());
  ici.ppEnabledExtensionNames = inst_exts.data();
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
    drm::println(stderr, "vkCreateInstance failed (libvulkan present? ICDs configured?)");
    return EXIT_FAILURE;
  }

  VkPhysicalDevice pd = pick_physical_device(instance, device.fd());
  if (pd == VK_NULL_HANDLE) {
    drm::println(stderr, "vulkan_scene: no VkPhysicalDevice matched the DRM fd");
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }

  // Queue family — any with graphics capability will do; vkCmdClear
  // doesn't need a dedicated transfer queue on any current driver.
  std::uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
  std::uint32_t qf_index = 0xFFFFFFFFU;
  for (std::uint32_t i = 0; i < qf_count; ++i) {
    if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
      qf_index = i;
      break;
    }
  }
  if (qf_index == 0xFFFFFFFFU) {
    drm::println(stderr, "vulkan_scene: no graphics queue on the picked device");
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }

  const float qp = 1.0F;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = qf_index;
  qci.queueCount = 1;
  qci.pQueuePriorities = &qp;

  const std::array<const char*, 4> dev_exts{
      "VK_KHR_external_memory",
      "VK_KHR_external_memory_fd",
      "VK_EXT_external_memory_dma_buf",
      "VK_EXT_image_drm_format_modifier",
  };
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<std::uint32_t>(dev_exts.size());
  dci.ppEnabledExtensionNames = dev_exts.data();
  VkDevice vk_device = VK_NULL_HANDLE;
  if (vkCreateDevice(pd, &dci, nullptr, &vk_device) != VK_SUCCESS) {
    drm::println(
        stderr,
        "vulkan_scene: vkCreateDevice failed — driver missing one of "
        "{{VK_KHR_external_memory, VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf, "
        "VK_EXT_image_drm_format_modifier}}");
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(vk_device, qf_index, 0, &queue);

  // Allocate a VkImage with the LINEAR DRM modifier. The single-
  // modifier list is what VK_EXT_image_drm_format_modifier needs; a
  // production renderer would intersect against
  // LayerScene::candidate_modifiers(DRM_FORMAT_ARGB8888) and pick.
  const std::uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  std::array<std::uint64_t, 1> modifiers{modifier};
  VkImageDrmFormatModifierListCreateInfoEXT drm_list{};
  drm_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
  drm_list.drmFormatModifierCount = 1;
  drm_list.pDrmFormatModifiers = modifiers.data();

  VkExternalMemoryImageCreateInfo ext_img{};
  ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  ext_img.pNext = &drm_list;

  VkImageCreateInfo ici_img{};
  ici_img.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici_img.pNext = &ext_img;
  ici_img.imageType = VK_IMAGE_TYPE_2D;
  ici_img.format = VK_FORMAT_B8G8R8A8_UNORM;  // matches DRM_FORMAT_ARGB8888 byte order
  ici_img.extent = {fb_w, fb_h, 1};
  ici_img.mipLevels = 1;
  ici_img.arrayLayers = 1;
  ici_img.samples = VK_SAMPLE_COUNT_1_BIT;
  ici_img.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  ici_img.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici_img.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici_img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage vk_image = VK_NULL_HANDLE;
  if (vkCreateImage(vk_device, &ici_img, nullptr, &vk_image) != VK_SUCCESS) {
    drm::println(stderr, "vulkan_scene: vkCreateImage (DRM-modifier ARGB8888 LINEAR) failed");
    vkDestroyDevice(vk_device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }

  VkMemoryRequirements mr{};
  vkGetImageMemoryRequirements(vk_device, vk_image, &mr);
  VkExportMemoryAllocateInfo emai{};
  emai.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &emai;
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex =
      find_memory_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == 0xFFFFFFFFU) {
    drm::println(stderr, "vulkan_scene: no DEVICE_LOCAL memory type for the exportable image");
    vkDestroyImage(vk_device, vk_image, nullptr);
    vkDestroyDevice(vk_device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }
  VkDeviceMemory vk_mem = VK_NULL_HANDLE;
  if (vkAllocateMemory(vk_device, &mai, nullptr, &vk_mem) != VK_SUCCESS) {
    drm::println(stderr, "vulkan_scene: vkAllocateMemory failed");
    vkDestroyImage(vk_device, vk_image, nullptr);
    vkDestroyDevice(vk_device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }
  vkBindImageMemory(vk_device, vk_image, vk_mem, 0);

  // Export the memory as a DMA-BUF fd.
  auto get_memory_fd =
      reinterpret_cast<PFN_vkGetMemoryFdKHR>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          vkGetDeviceProcAddr(vk_device, "vkGetMemoryFdKHR"));
  if (get_memory_fd == nullptr) {
    drm::println(stderr, "vulkan_scene: vkGetMemoryFdKHR not exported");
    vkFreeMemory(vk_device, vk_mem, nullptr);
    vkDestroyImage(vk_device, vk_image, nullptr);
    vkDestroyDevice(vk_device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }
  VkMemoryGetFdInfoKHR mgfi{};
  mgfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  mgfi.memory = vk_mem;
  mgfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  int dmabuf_fd = -1;
  if (get_memory_fd(vk_device, &mgfi, &dmabuf_fd) != VK_SUCCESS || dmabuf_fd < 0) {
    drm::println(stderr, "vulkan_scene: vkGetMemoryFdKHR failed");
    vkFreeMemory(vk_device, vk_mem, nullptr);
    vkDestroyImage(vk_device, vk_image, nullptr);
    vkDestroyDevice(vk_device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }

  // Query the image's subresource layout so we know the row pitch
  // and offset to feed into ExternalDmaBufSource.
  VkImageSubresource sub{};
  sub.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
  VkSubresourceLayout layout{};
  vkGetImageSubresourceLayout(vk_device, vk_image, &sub, &layout);

  drm::scene::ExternalPlaneInfo plane_info{};
  plane_info.fd = dmabuf_fd;
  plane_info.pitch = static_cast<std::uint32_t>(layout.rowPitch);
  plane_info.offset = static_cast<std::uint32_t>(layout.offset);
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{plane_info};

  auto vk_source = drm::scene::ExternalDmaBufSource::create(device, fb_w, fb_h, DRM_FORMAT_ARGB8888,
                                                              modifier, planes);
  // ExternalDmaBufSource dups the fd; we close ours now.
  ::close(dmabuf_fd);
  if (!vk_source) {
    drm::println(stderr, "ExternalDmaBufSource::create: {}", vk_source.error().message());
    vkFreeMemory(vk_device, vk_mem, nullptr);
    vkDestroyImage(vk_device, vk_image, nullptr);
    vkDestroyDevice(vk_device, nullptr);
    vkDestroyInstance(instance, nullptr);
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
  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.queueFamilyIndex = qf_index;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  vkCreateCommandPool(vk_device, &pci, nullptr, &cmd_pool);

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(vk_device, &cbai, &cmd);

  // First commit: brings up modeset + presents the Vulkan image
  // before we've rendered. The image's INITIAL_LAYOUT is UNDEFINED
  // → the kernel sees zero-initialized pixels; visible briefly until
  // the first clear lands.
  if (auto r = scene->commit(); !r) {
    drm::println(stderr, "first commit: {}", r.error().message());
    return EXIT_FAILURE;
  }

  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const auto deadline = t0 + std::chrono::seconds(args.seconds);
  std::uint64_t frames = 0;
  bool first = true;
  while (clk::now() < deadline) {
    const float t = std::chrono::duration<float>(clk::now() - t0).count();
    VkClearColorValue clear{};
    clear.float32[0] = 0.5F + (0.5F * std::sin(t * 1.0F));
    clear.float32[1] = 0.5F + (0.5F * std::sin((t * 1.3F) + 2.0F));
    clear.float32[2] = 0.5F + (0.5F * std::sin((t * 1.7F) + 4.0F));
    clear.float32[3] = 1.0F;

    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbi);

    // Transition UNDEFINED → GENERAL on the first iteration, then
    // hold GENERAL across re-clears. GENERAL is the right layout for
    // the vkCmdClearColorImage path; TRANSFER_DST_OPTIMAL would also
    // work but requires another transition after each clear.
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = vk_image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);

    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, vk_image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    // Single-buffered: idle the queue before letting KMS scan out.
    // A real app would use an IN_FENCE_FD-backed sync_file to chain
    // the GPU completion into the atomic commit.
    vkQueueWaitIdle(queue);

    if (auto r = scene->commit(); !r) {
      drm::println(stderr, "commit: {}", r.error().message());
      break;
    }
    ++frames;
    first = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  drm::println("vulkan_scene: {} frames in {}s", frames, args.seconds);

  // Tear-down. Scene first (releases the dma-buf source's fb_id +
  // GEM handle); then Vulkan.
  scene.reset();
  vkDestroyCommandPool(vk_device, cmd_pool, nullptr);
  vkFreeMemory(vk_device, vk_mem, nullptr);
  vkDestroyImage(vk_device, vk_image, nullptr);
  vkDestroyDevice(vk_device, nullptr);
  vkDestroyInstance(instance, nullptr);
  return EXIT_SUCCESS;
}