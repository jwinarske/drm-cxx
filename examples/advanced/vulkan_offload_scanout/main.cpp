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
// Run:  ./vulkan_offload_scanout [display=/dev/dri/card0]

#include "../../common/kms_present.hpp"

#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <vulkan/vulkan_core.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fmt = drm::fmt;

namespace {

#define VK_CHECK(x)                                                     \
  if (const VkResult vr_ = (x); vr_ != VK_SUCCESS) {                    \
    std::fprintf(stderr, "%s failed: %d\n", #x, static_cast<int>(vr_)); \
    return 1;                                                           \
  }

template <typename Fn>
Fn load(VkInstance inst, const char* name) {
  return reinterpret_cast<Fn>(vkGetInstanceProcAddr(inst, name));
}

}  // namespace

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
  // Vulkan renders BGRA; try ARGB8888 first, then XRGB8888 on the display side.
  const VkFormat vk_format = VK_FORMAT_B8G8R8A8_UNORM;
  std::uint32_t kms_fourcc = DRM_FORMAT_ARGB8888;
  if (disp_tbl->modifiers_for(kms_fourcc).empty()) {
    kms_fourcc = DRM_FORMAT_XRGB8888;
  }
  std::printf("display %s: crtc %u, plane %u, %ux%u, fourcc '%c%c%c%c'\n", disp_path,
              target->crtc_id, target->primary_plane, w, h, char(kms_fourcc), char(kms_fourcc >> 8),
              char(kms_fourcc >> 16), char(kms_fourcc >> 24));

  // --- Vulkan instance + device with dma-buf export ------------------------
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance inst = VK_NULL_HANDLE;
  VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

  std::uint32_t ngpu = 0;
  vkEnumeratePhysicalDevices(inst, &ngpu, nullptr);
  if (ngpu == 0U) {
    std::fprintf(stderr, "no Vulkan device\n");
    return 1;
  }
  std::vector<VkPhysicalDevice> gpus(ngpu);
  vkEnumeratePhysicalDevices(inst, &ngpu, gpus.data());
  // [0] for brevity; match VK_EXT_physical_device_drm to the display node on
  // multi-GPU systems.
  VkPhysicalDevice gpu = gpus[0];

  std::uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, nullptr);
  std::vector<VkQueueFamilyProperties> qf(nq);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, qf.data());
  std::uint32_t qfi = 0;
  for (std::uint32_t i = 0; i < nq; ++i) {
    if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
      qfi = i;
      break;
    }
  }

  const float prio = 1.0F;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = qfi;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  const char* const dev_exts[] = {
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
  };
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<std::uint32_t>(std::size(dev_exts));
  dci.ppEnabledExtensionNames = dev_exts;
  VkDevice dev = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &dev));
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev, qfi, 0, &queue);

  auto p_get_modifier_props = load<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
      inst, "vkGetImageDrmFormatModifierPropertiesEXT");
  auto p_get_memory_fd = load<PFN_vkGetMemoryFdKHR>(inst, "vkGetMemoryFdKHR");
  if ((p_get_modifier_props == nullptr) || (p_get_memory_fd == nullptr)) {
    std::fprintf(stderr, "missing required device entry points\n");
    return 1;
  }

  // --- candidate modifiers: device-renderable INTERSECT display-scannable ---
  VkDrmFormatModifierPropertiesListEXT mod_list{
      VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 fp{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  fp.pNext = &mod_list;
  vkGetPhysicalDeviceFormatProperties2(gpu, vk_format, &fp);
  std::vector<VkDrmFormatModifierPropertiesEXT> mod_props(mod_list.drmFormatModifierCount);
  mod_list.pDrmFormatModifierProperties = mod_props.data();
  vkGetPhysicalDeviceFormatProperties2(gpu, vk_format, &fp);

  const VkFormatFeatureFlags need =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  const bool dump_mods = std::getenv("DRM_FMT_DUMP_VK_MODS") != nullptr;
  std::vector<fmt::Modifier> candidates;
  for (const auto& mp : mod_props) {
    const fmt::Modifier m{mp.drmFormatModifier};
    const bool renderable = (mp.drmFormatModifierTilingFeatures & need) == need;
    const bool scannable = disp_tbl->supports(kms_fourcc, m);
    if (dump_mods) {
      std::printf("  VK %-40s render=%d display=%d\n", fmt::describe(m).c_str(), renderable ? 1 : 0,
                  scannable ? 1 : 0);
    }
    if (renderable && scannable) {
      candidates.push_back(m);
    }
  }
  if (candidates.empty()) {
    std::fprintf(stderr,
                 "no modifier both the GPU can render and the display can scan "
                 "out for this format\n");
    return 1;
  }
  // COMPRESSION first, then tiling, LINEAR last (stable to keep VK's order within
  // a class).
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

  // Diagnostic: force the driver onto a compression-only modifier list, to prove
  // a real dcc=1 buffer can be both rendered and scanned out (the driver otherwise
  // prefers plain tiling for a fresh render target). Set DRM_FMT_FORCE_COMPRESSION=1.
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

  // --- create the modifier image, exportable -------------------------------
  VkImageDrmFormatModifierListCreateInfoEXT mod_ci{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
  mod_ci.drmFormatModifierCount = static_cast<std::uint32_t>(cand_vals.size());
  mod_ci.pDrmFormatModifiers = cand_vals.data();

  VkExternalMemoryImageCreateInfo ext_ci{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  ext_ci.pNext = &mod_ci;
  ext_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkImageCreateInfo img_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  img_ci.pNext = &ext_ci;
  img_ci.imageType = VK_IMAGE_TYPE_2D;
  img_ci.format = vk_format;
  img_ci.extent = {w, h, 1};
  img_ci.mipLevels = 1;
  img_ci.arrayLayers = 1;
  img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  img_ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImage(dev, &img_ci, nullptr, &image));

  VkMemoryRequirements mr{};
  vkGetImageMemoryRequirements(dev, image, &mr);
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
  const drm::span<const VkMemoryType> types{mp.memoryTypes, mp.memoryTypeCount};
  std::uint32_t mem_type = 0;
  for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (((mr.memoryTypeBits & (1U << i)) != 0U) &&
        ((types[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U)) {
      mem_type = i;
      break;
    }
  }

  VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.pNext = &exp;
  ded.image = image;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &ded;
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = mem_type;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem));
  VK_CHECK(vkBindImageMemory(dev, image, mem, 0));

  // chosen modifier + per-plane layout
  VkImageDrmFormatModifierPropertiesEXT chosen_props{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
  VK_CHECK(p_get_modifier_props(dev, image, &chosen_props));
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
    VkImageSubresource sub{};
    sub.aspectMask =
        static_cast<VkImageAspectFlagBits>(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i);
    VkSubresourceLayout sl{};
    vkGetImageSubresourceLayout(dev, image, &sub, &sl);
    planes[i].stride = static_cast<std::uint32_t>(sl.rowPitch);
    planes[i].offset = static_cast<std::uint32_t>(sl.offset);
  }

  // --- record: clear, then release to the foreign (display) queue ----------
  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpci.queueFamilyIndex = qfi;
  VkCommandPool pool = VK_NULL_HANDLE;
  VK_CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  to_general.srcAccessMask = 0;
  to_general.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_general.image = image;
  to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &to_general);

  VkClearColorValue color{};
  color.float32[0] = 0.20F;
  color.float32[1] = 0.55F;
  color.float32[2] = 0.85F;
  color.float32[3] = 1.0F;
  VkImageSubresourceRange const range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);

  // Release ownership to the foreign queue: the display engine reads it outside
  // Vulkan's queue model.
  VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  release.dstAccessMask = 0;
  release.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  release.srcQueueFamilyIndex = qfi;
  release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  release.image = image;
  release.subresourceRange = range;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &release);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(queue));  // retire before the display reads it

  // --- export the backing memory once; all planes share the fd at offsets ---
  VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  gfi.memory = mem;
  gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  int dmabuf = -1;
  VK_CHECK(p_get_memory_fd(dev, &gfi, &dmabuf));
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

  // --- ground truth + present ---------------------------------------------
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
  if (int const r = kms::commit_fb(disp_fd, *target, fb->fb_id(), DRM_MODE_ATOMIC_ALLOW_MODESET)) {
    std::fprintf(stderr, "present: %s\n", std::strerror(-r));
    return 1;
  }
  std::printf("on screen (Vulkan-rendered %s scanned out by the display) -- 3s\n",
              fmt::describe(chosen).c_str());
  sleep(3);

  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyImage(dev, image, nullptr);
  vkFreeMemory(dev, mem, nullptr);
  vkDestroyDevice(dev, nullptr);
  vkDestroyInstance(inst, nullptr);
  // `fb` borrows disp_fd for its RmFB on teardown, so release it before the fd
  // is closed rather than at function-scope end.
  {
    const fmt::ScanoutBuffer released = std::move(*fb);
  }
  close(disp_fd);
  return 0;
}