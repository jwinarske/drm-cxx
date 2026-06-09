// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/vk_scanout_producer.cpp

#include <drm-cxx/present/vk_scanout_producer.hpp>

#if DRM_CXX_HAS_VULKAN

// Vulkan-Hpp dynamic dispatch: no prototypes, libvulkan dlopen'd at runtime.
#define VK_NO_PROTOTYPES
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) -- required vulkan.hpp config knob
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/log.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <drm_fourcc.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

// One translation unit must hold the dynamic dispatcher storage.
// NOLINTNEXTLINE(misc-include-cleaner) -- macro from the included vulkan.hpp
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace drm::present {

namespace {

[[nodiscard]] std::error_code err(std::errc code) noexcept {
  return std::make_error_code(code);
}

// Vulkan memory-plane aspects, indexed by DRM plane (multi-plane modifiers).
constexpr std::array<vk::ImageAspectFlagBits, 4> memory_planes{
    vk::ImageAspectFlagBits::eMemoryPlane0EXT, vk::ImageAspectFlagBits::eMemoryPlane1EXT,
    vk::ImageAspectFlagBits::eMemoryPlane2EXT, vk::ImageAspectFlagBits::eMemoryPlane3EXT};

// DRM fourcc -> Vulkan format. ARGB8888 / XRGB8888 are little-endian BGRA byte
// order == VK_FORMAT_B8G8R8A8_UNORM. Returns eUndefined for unsupported fourccs.
[[nodiscard]] vk::Format vk_format_for(std::uint32_t fourcc) noexcept {
  switch (fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
      return vk::Format::eB8G8R8A8Unorm;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
      return vk::Format::eR8G8B8A8Unorm;
    default:
      return vk::Format::eUndefined;
  }
}

[[nodiscard]] std::uint32_t find_device_local_memory(const vk::PhysicalDevice& physical,
                                                     std::uint32_t type_bits) noexcept {
  const vk::PhysicalDeviceMemoryProperties props = physical.getMemoryProperties();
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    const bool allowed = (type_bits & (1U << i)) != 0U;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    const vk::MemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
    const bool device_local = static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eDeviceLocal);
    if (allowed && device_local) {
      return i;
    }
  }
  return UINT32_MAX;
}

}  // namespace

struct VkScanoutProducer::Impl {
  drm::Device* dev{nullptr};
  vk::detail::DynamicLoader loader;
  vk::Instance instance;
  vk::PhysicalDevice physical;
  vk::Device device;
  vk::Queue queue;
  std::uint32_t queue_family{0};
  vk::CommandPool cmd_pool;
  vk::CommandBuffer cmd;
  vk::Image image;
  vk::DeviceMemory memory;
  vk::Extent2D extent;
  bool first_frame{true};
  // Non-owning: the scene owns the source; the producer outlives the scene
  // (its VkImage memory backs the dmabuf), so this stays valid.
  scene::ExternalDmaBufSource* vk_source{nullptr};
  vk::Semaphore export_sem;  // signaled by each render submit, exported as sync_file
  vk::Fence reuse_fence;     // CPU-waited before re-recording (image + cmd reuse)

  ~Impl() {
    try {
      if (device) {
        device.waitIdle();
        if (image) {
          device.destroyImage(image);
        }
        if (memory) {
          device.freeMemory(memory);
        }
        if (export_sem) {
          device.destroySemaphore(export_sem);
        }
        if (reuse_fence) {
          device.destroyFence(reuse_fence);
        }
        if (cmd_pool) {
          device.destroyCommandPool(cmd_pool);
        }
        device.destroy();
      }
      if (instance) {
        instance.destroy();
      }
    } catch (const std::exception& e) {  // dtor must not throw
      drm::log_warn("VkScanoutProducer: teardown: {}", e.what());
    }
  }
};

VkScanoutProducer::VkScanoutProducer() = default;
VkScanoutProducer::~VkScanoutProducer() = default;

drm::expected<std::unique_ptr<VkScanoutProducer>, std::error_code> VkScanoutProducer::create(
    drm::Device& dev) {
  auto impl = std::make_unique<Impl>();
  impl->dev = &dev;

  // Match the Vulkan physical device to the DRM node `dev` drives, by its
  // primary major:minor (VkPhysicalDeviceDrmPropertiesEXT).
  struct ::stat st{};
  if (::fstat(dev.fd(), &st) != 0) {
    return drm::unexpected<std::error_code>(err(std::errc::bad_file_descriptor));
  }
  const auto want_major = static_cast<std::int64_t>(major(st.st_rdev));
  const auto want_minor = static_cast<std::int64_t>(minor(st.st_rdev));

  try {
    auto gipa = impl->loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    // NOLINTNEXTLINE(misc-include-cleaner) -- VULKAN_HPP_DEFAULT_DISPATCHER from vulkan.hpp
    VULKAN_HPP_DEFAULT_DISPATCHER.init(gipa);

    const vk::ApplicationInfo app{"drm-cxx", 0, "drm-cxx", 0, VK_API_VERSION_1_1};
    const std::array<const char*, 2> inst_exts{"VK_KHR_get_physical_device_properties2",
                                               "VK_KHR_external_memory_capabilities"};
    impl->instance = vk::createInstance(
        vk::InstanceCreateInfo{}.setPApplicationInfo(&app).setPEnabledExtensionNames(inst_exts));
    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl->instance);

    vk::PhysicalDevice cross_device_fallback;
    for (const vk::PhysicalDevice& candidate : impl->instance.enumeratePhysicalDevices()) {
      if (!cross_device_fallback) {
        cross_device_fallback = candidate;  // first Vulkan device (the GPU)
      }
      auto chain =
          candidate
              .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDrmPropertiesEXT>();
      const auto& drm_props = chain.get<vk::PhysicalDeviceDrmPropertiesEXT>();
      if ((drm_props.hasPrimary == VK_TRUE) && (drm_props.primaryMajor == want_major) &&
          (drm_props.primaryMinor == want_minor)) {
        impl->physical = candidate;
        break;
      }
    }
    if (!impl->physical) {
      // No Vulkan device drives this KMS node. On a split render/display SoC
      // (e.g. RK3588: PanVK on the panthor render node, scanout on rockchip) the
      // GPU is a separate device — render on it and let the buffer cross to the
      // KMS device as a dmabuf (ExternalDmaBufSource imports it on impl->dev).
      // Cross-device scanout usually needs a LINEAR / otherwise-common modifier;
      // the backend's negotiation against the plane's IN_FORMATS handles that.
      impl->physical = cross_device_fallback;
      if (impl->physical) {
        drm::log_info(
            "VkScanoutProducer: no Vulkan device for KMS node {}:{}; using a separate render "
            "device (cross-device scanout via dmabuf)",
            want_major, want_minor);
      }
    }
    if (!impl->physical) {
      return drm::unexpected<std::error_code>(err(std::errc::no_such_device));
    }

    // Graphics queue family.
    const auto families = impl->physical.getQueueFamilyProperties();
    std::uint32_t qf = UINT32_MAX;
    for (std::uint32_t i = 0; i < families.size(); ++i) {
      if (families[i].queueFlags & vk::QueueFlagBits::eGraphics) {
        qf = i;
        break;
      }
    }
    if (qf == UINT32_MAX) {
      return drm::unexpected<std::error_code>(err(std::errc::not_supported));
    }
    impl->queue_family = qf;

    const float prio = 1.0F;
    const vk::DeviceQueueCreateInfo qci{{}, qf, 1, &prio};
    const std::array<const char*, 6> dev_exts{
        "VK_KHR_external_memory",         "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier",
        "VK_KHR_external_semaphore",      "VK_KHR_external_semaphore_fd"};
    impl->device = impl->physical.createDevice(
        vk::DeviceCreateInfo{}.setQueueCreateInfos(qci).setPEnabledExtensionNames(dev_exts));
    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl->device);
    impl->queue = impl->device.getQueue(qf, 0);

    impl->cmd_pool = impl->device.createCommandPool(
        vk::CommandPoolCreateInfo{vk::CommandPoolCreateFlagBits::eResetCommandBuffer, qf});
    impl->cmd = impl->device
                    .allocateCommandBuffers(vk::CommandBufferAllocateInfo{
                        impl->cmd_pool, vk::CommandBufferLevel::ePrimary, 1})
                    .front();

    // A semaphore signaled by each render submit and exported as a sync_file
    // (the acquire fence KMS waits on), plus a fence for the CPU reuse-wait.
    vk::StructureChain<vk::SemaphoreCreateInfo, vk::ExportSemaphoreCreateInfo> sem_chain{
        vk::SemaphoreCreateInfo{},
        vk::ExportSemaphoreCreateInfo{vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd}};
    impl->export_sem = impl->device.createSemaphore(sem_chain.get<vk::SemaphoreCreateInfo>());
    impl->reuse_fence = impl->device.createFence(vk::FenceCreateInfo{});
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer::create: {}", e.what());
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  auto producer = std::unique_ptr<VkScanoutProducer>(new VkScanoutProducer());
  producer->impl_ = std::move(impl);
  return producer;
}

std::vector<std::uint64_t> VkScanoutProducer::exportable_modifiers(std::uint32_t fourcc) {
  std::vector<std::uint64_t> out;
  const vk::Format format = vk_format_for(fourcc);
  if ((format == vk::Format::eUndefined) || !impl_->physical) {
    return out;
  }
  try {
    vk::DrmFormatModifierPropertiesListEXT list;
    vk::FormatProperties2 fp;
    fp.pNext = &list;
    impl_->physical.getFormatProperties2(format, &fp);
    std::vector<vk::DrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = props.data();
    impl_->physical.getFormatProperties2(format, &fp);
    for (const auto& mod : props) {
      if (mod.drmFormatModifierTilingFeatures & vk::FormatFeatureFlagBits::eColorAttachment) {
        out.push_back(mod.drmFormatModifier);
      }
    }
  } catch (const std::exception&) {
    out.clear();
  }
  return out;
}

drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
VkScanoutProducer::create_buffer(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                                 drm::span<const std::uint64_t> allowed) {
  if (impl_->image) {
    return drm::unexpected<std::error_code>(err(std::errc::already_connected));
  }
  const vk::Format format = vk_format_for(fourcc);
  if (format == vk::Format::eUndefined) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }

  // The backend hands us the negotiated set (Vulkan-renderable ∩ plane); offer
  // it (or LINEAR) to the modifier-list image-create. Vulkan picks one.
  std::vector<std::uint64_t> mods(allowed.begin(), allowed.end());
  if (mods.empty()) {
    mods.push_back(DRM_FORMAT_MOD_LINEAR);
  }

  int dmabuf_fd = -1;
  std::uint64_t chosen_modifier = DRM_FORMAT_MOD_INVALID;
  std::vector<scene::ExternalPlaneInfo> planes;
  try {
    vk::StructureChain<vk::ImageCreateInfo, vk::ExternalMemoryImageCreateInfo,
                       vk::ImageDrmFormatModifierListCreateInfoEXT>
        image_chain{
            vk::ImageCreateInfo{}
                .setImageType(vk::ImageType::e2D)
                .setFormat(format)
                .setExtent({width, height, 1})
                .setMipLevels(1)
                .setArrayLayers(1)
                .setSamples(vk::SampleCountFlagBits::e1)
                .setTiling(vk::ImageTiling::eDrmFormatModifierEXT)
                .setUsage(vk::ImageUsageFlagBits::eColorAttachment |
                          vk::ImageUsageFlagBits::eTransferDst)
                .setSharingMode(vk::SharingMode::eExclusive)
                .setInitialLayout(vk::ImageLayout::eUndefined),
            vk::ExternalMemoryImageCreateInfo{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT},
            vk::ImageDrmFormatModifierListCreateInfoEXT{}.setDrmFormatModifiers(mods)};
    impl_->image = impl_->device.createImage(image_chain.get<vk::ImageCreateInfo>());

    const vk::MemoryRequirements mr = impl_->device.getImageMemoryRequirements(impl_->image);
    const std::uint32_t type_index = find_device_local_memory(impl_->physical, mr.memoryTypeBits);
    if (type_index == UINT32_MAX) {
      return drm::unexpected<std::error_code>(err(std::errc::not_supported));
    }
    vk::StructureChain<vk::MemoryAllocateInfo, vk::ExportMemoryAllocateInfo> alloc_chain{
        vk::MemoryAllocateInfo{mr.size, type_index},
        vk::ExportMemoryAllocateInfo{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT}};
    impl_->memory = impl_->device.allocateMemory(alloc_chain.get<vk::MemoryAllocateInfo>());
    impl_->device.bindImageMemory(impl_->image, impl_->memory, 0);

    dmabuf_fd = impl_->device.getMemoryFdKHR(
        vk::MemoryGetFdInfoKHR{impl_->memory, vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT});

    // The actual modifier Vulkan assigned, and its plane count.
    chosen_modifier =
        impl_->device.getImageDrmFormatModifierPropertiesEXT(impl_->image).drmFormatModifier;
    std::uint32_t plane_count = 1;
    {
      vk::DrmFormatModifierPropertiesListEXT list;
      vk::FormatProperties2 fp;
      fp.pNext = &list;
      impl_->physical.getFormatProperties2(format, &fp);
      std::vector<vk::DrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
      list.pDrmFormatModifierProperties = props.data();
      impl_->physical.getFormatProperties2(format, &fp);
      for (const auto& mod : props) {
        if (mod.drmFormatModifier == chosen_modifier) {
          plane_count = mod.drmFormatModifierPlaneCount;
          break;
        }
      }
    }

    for (std::uint32_t p = 0; (p < plane_count) && (p < memory_planes.size()); ++p) {
      const vk::SubresourceLayout layout = impl_->device.getImageSubresourceLayout(
          impl_->image, vk::ImageSubresource{memory_planes.at(p)});
      planes.push_back(scene::ExternalPlaneInfo{dmabuf_fd,
                                                static_cast<std::uint32_t>(layout.offset),
                                                static_cast<std::uint32_t>(layout.rowPitch)});
    }
  } catch (const std::exception& e) {
    if (dmabuf_fd >= 0) {
      ::close(dmabuf_fd);
    }
    impl_->image = nullptr;
    impl_->memory = nullptr;
    drm::log_warn("VkScanoutProducer::create_buffer: {}", e.what());
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  auto source = scene::ExternalDmaBufSource::create(*impl_->dev, width, height, fourcc,
                                                    chosen_modifier, planes);
  ::close(dmabuf_fd);  // ExternalDmaBufSource dups the fd
  if (!source) {
    return drm::unexpected<std::error_code>(source.error());
  }
  impl_->extent = vk::Extent2D{width, height};
  // Keep a non-owning handle so render_clear can stash the acquire fence on it.
  impl_->vk_source = (*source).get();
  return std::unique_ptr<scene::LayerBufferSource>(std::move(*source));
}

drm::expected<void, std::error_code> VkScanoutProducer::render_clear(std::array<float, 4> rgba) {
  if (!impl_->image) {
    return drm::unexpected<std::error_code>(err(std::errc::not_connected));
  }
  try {
    if (!impl_->first_frame) {
      // Wait for the previous submit before reusing the command buffer and
      // re-rendering into the single image (buffer-reuse safety; OUT_FENCE-gated
      // double-buffering is a follow-up). This is a 1-frame-behind CPU gate, not
      // a wait on the current frame — the commit still overlaps this submit.
      (void)impl_->device.waitForFences(impl_->reuse_fence, VK_TRUE, UINT64_MAX);
      impl_->device.resetFences(impl_->reuse_fence);
    }
    impl_->cmd.reset();
    impl_->cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier to_general{
        {},
        vk::AccessFlagBits::eTransferWrite,
        impl_->first_frame ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        impl_->image,
        range};
    impl_->cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_general);

    const vk::ClearColorValue clear{std::array<float, 4>{rgba[0], rgba[1], rgba[2], rgba[3]}};
    impl_->cmd.clearColorImage(impl_->image, vk::ImageLayout::eGeneral, clear, range);
    impl_->cmd.end();

    // Submit WITHOUT a CPU wait: signal the export semaphore (-> sync_file the
    // scene hands KMS as IN_FENCE_FD) and the reuse fence (next-frame gate).
    impl_->queue.submit(
        vk::SubmitInfo{}.setCommandBuffers(impl_->cmd).setSignalSemaphores(impl_->export_sem),
        impl_->reuse_fence);

    // Export the semaphore's pending signal as a sync_file and stash it on the
    // source as this frame's acquire fence. import_fd dups, so close ours.
    const int sem_fd = impl_->device.getSemaphoreFdKHR(vk::SemaphoreGetFdInfoKHR{
        impl_->export_sem, vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd});
    auto fence = drm::sync::SyncFence::import_fd(sem_fd);
    if (sem_fd >= 0) {
      ::close(sem_fd);
    }
    if (fence && (impl_->vk_source != nullptr)) {
      impl_->vk_source->set_acquire_fence(std::move(*fence));
    } else if (!fence) {
      drm::log_warn("VkScanoutProducer::render_clear: sync_file import failed: {}",
                    fence.error().message());
    }
    impl_->first_frame = false;
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer::render_clear: {}", e.what());
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  return {};
}

void* VkScanoutProducer::vk_device() const noexcept {
  return static_cast<VkDevice>(impl_->device);
}
void* VkScanoutProducer::vk_queue() const noexcept {
  return static_cast<VkQueue>(impl_->queue);
}
void* VkScanoutProducer::vk_image() const noexcept {
  return static_cast<VkImage>(impl_->image);
}
std::uint32_t VkScanoutProducer::queue_family_index() const noexcept {
  return impl_->queue_family;
}

}  // namespace drm::present

#endif  // DRM_CXX_HAS_VULKAN
