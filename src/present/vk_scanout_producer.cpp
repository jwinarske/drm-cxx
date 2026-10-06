// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/vk_scanout_producer.cpp

#include <drm-cxx/present/vk_scanout_producer.hpp>

#if DRM_CXX_HAS_VK_SCANOUT_PRODUCER

// Vulkan-Hpp dynamic dispatch: no prototypes, libvulkan dlopen'd at runtime.
#define VK_NO_PROTOTYPES
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) -- required vulkan.hpp config knob
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/gbm/buffer.hpp>
#include <drm-cxx/gbm/device.hpp>
#include <drm-cxx/log.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_ring.hpp>
#include <drm-cxx/sync/fence.hpp>

#if DRM_CXX_HAS_EGL
#include "gl_blit.hpp"
#endif

#include <drm.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <xf86drm.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
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

const vk::ImageSubresourceRange k_color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

// Every scanout image: drawn into (render pass or transfer) and copied from
// (the copy tier's read-back, the Vivante resolve).
constexpr vk::ImageUsageFlags k_image_usage = vk::ImageUsageFlagBits::eColorAttachment |
                                              vk::ImageUsageFlagBits::eTransferDst |
                                              vk::ImageUsageFlagBits::eTransferSrc;

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

// First memory type allowed by `type_bits` that has all of `want`; UINT32_MAX if none.
[[nodiscard]] std::uint32_t find_memory_type(const vk::PhysicalDevice& physical,
                                             std::uint32_t type_bits,
                                             vk::MemoryPropertyFlags want) noexcept {
  const vk::PhysicalDeviceMemoryProperties props = physical.getMemoryProperties();
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    const vk::MemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
    if (((type_bits & (1U << i)) != 0U) && ((flags & want) == want)) {
      return i;
    }
  }
  return UINT32_MAX;
}

// One display-side allocation: an owner (GBM or dumb buffer) plus the dma-buf
// fd, pitch and size Vulkan imports.
struct DisplayAlloc {
  gbm::Buffer gbm;
  dumb::Buffer dumb;
  int fd{-1};
  std::uint32_t pitch{0};
  std::size_t size{0};
};

// Allocate `rows` x `width` LINEAR on the KMS device. GBM first: on stacks whose
// GBM backend allocates through the GPU driver (e.g. i.MX/Vivante), that memory
// is both scannable and native to the GPU, so a Vulkan import aliases it. A dumb
// buffer is the fallback — scannable, but a GPU driver may not alias foreign
// pages on import.
[[nodiscard]] std::optional<DisplayAlloc> alloc_display(const drm::Device& dev,
                                                        std::optional<gbm::GbmDevice>& gbm_dev,
                                                        std::uint32_t width, std::uint32_t rows,
                                                        std::uint32_t fourcc) {
  DisplayAlloc a;
  if (!gbm_dev.has_value()) {
    if (auto g = gbm::GbmDevice::create(dev.fd()); g) {
      gbm_dev.emplace(std::move(*g));
    }
  }
  if (gbm_dev.has_value()) {
    gbm::Config cfg;
    cfg.width = width;
    cfg.height = rows;
    cfg.drm_format = fourcc;
    cfg.usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
    cfg.modifier = DRM_FORMAT_MOD_LINEAR;
    cfg.add_fb = false;
    if (auto bo = gbm::Buffer::create(*gbm_dev, cfg); bo) {
      if (auto fd = bo->fd(); fd && *fd >= 0) {
        a.fd = *fd;
        a.pitch = bo->stride();
        a.size = static_cast<std::size_t>(bo->stride()) * rows;
        a.gbm = std::move(*bo);
        return a;
      }
    }
  }
  auto buf = dumb::Buffer::create(dev, dumb::Config{width, rows, fourcc, 32, false});
  if (!buf) {
    return std::nullopt;
  }
  if (drmPrimeHandleToFD(dev.fd(), buf->handle(), DRM_CLOEXEC | DRM_RDWR, &a.fd) != 0 || a.fd < 0) {
    return std::nullopt;
  }
  a.pitch = buf->stride();
  a.size = buf->size_bytes();
  a.dumb = std::move(*buf);
  return a;
}

}  // namespace

struct VkScanoutProducer::Impl {
  // One scanout image and everything needed to render it independently of the
  // others: the export and import tiers rotate among buffer_count of these;
  // the blit and copy tiers use one.
  struct Slot {
    vk::Image image;
    vk::DeviceMemory memory;
    // Import tier: the display-side buffer the image is bound to.
    gbm::Buffer display_gbm;
    dumb::Buffer display_dumb;
    vk::CommandBuffer cmd;
    vk::Fence fence;  // signaled when the slot's last submit finished
    // Never rendered: layout UNDEFINED, not yet handed to the display.
    bool fresh{true};
    // Not held by the scene (cleared by render(), set again on release).
    std::atomic<bool> free{true};
    // Release fence handed back by the scene; waited before the next render.
    std::mutex mu;
    std::optional<drm::sync::SyncFence> release_fence;
  };

  drm::Device* dev{nullptr};
  std::uint32_t buffer_count{3};
  vk::detail::DynamicLoader loader;
  vk::Instance instance;
  vk::PhysicalDevice physical;
  vk::Device device;
  vk::Queue queue;
  std::uint32_t queue_family{0};
  // Queue family a scanout image is released to after each render and acquired
  // back from before the next: the display (or GL) is an external consumer.
  std::uint32_t foreign_family{VK_QUEUE_FAMILY_EXTERNAL};
  vk::CommandPool cmd_pool;
  vk::Semaphore export_sem;  // signaled by each render submit, exported as sync_file

  // How rendered frames reach the display (see create_buffer):
  //   Export — the display imports Vulkan's own buffers (zero-copy);
  //   Import — Vulkan imports display-side buffers (zero-copy, self-checked);
  //   Blit   — the display cannot reach Vulkan's buffer, but the GPU's GL
  //            stack can: each frame Vulkan's exported image is drawn by GL
  //            into a GlScanoutProducer's scannable surface (one GPU copy);
  //   Copy   — the frame is copied into host-visible memory and memcpy'd into
  //            one of two display-side dumb buffers.
  enum class Mode : std::uint8_t { Export, Import, Blit, Copy };
  Mode mode{Mode::Export};
  vk::Format format{vk::Format::eUndefined};
  vk::Extent2D extent;
  std::optional<gbm::GbmDevice> gbm_dev;  // import tier allocations
  // shared_ptr: the scene's release callbacks hold the slots too, and may run
  // after the producer is gone if the scene outlives it.
  std::vector<std::shared_ptr<Slot>> slots;
  std::size_t last_slot{0};
  // Non-owning: the scene owns the ring; the producer outlives the scene.
  scene::ExternalDmaBufRing* ring{nullptr};

  // Copy tier: host-visible staging buffer + two display-side dumb buffers.
  vk::Buffer copy_buffer;
  vk::DeviceMemory copy_memory;
  void* copy_mapped{nullptr};
  bool copy_coherent{true};
  std::array<dumb::Buffer, 2> copy_buffers;
  // Shared with the copy ring's release callback (see slots).
  std::shared_ptr<std::array<std::atomic<bool>, 2>> copy_slot_free{
      std::make_shared<std::array<std::atomic<bool>, 2>>()};
  std::size_t copy_next{0};

  // VeriSilicon/Vivante keeps cleared pixels of a tiled image in tile-status
  // (fast-clear) metadata and does not resolve them into memory on a release
  // to an external/foreign queue family: whatever was only cleared (not drawn
  // or copied) reads back as stale memory to the display, GL or the CPU. Any
  // transfer read of the image resolves the whole surface, so after each
  // render a one-pixel copy into kick_buffer forces it out. A
  // VK_IMAGE_TILING_LINEAR image (what LINEAR is allocated as here, see
  // linear_via_linear_tiling) has no tile status and does not need it; the
  // tiled modifiers do.
  bool resolve_kick{false};
  // VeriSilicon/Vivante also lays out a VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
  // image tiled even when the chosen modifier is DRM_FORMAT_MOD_LINEAR (and
  // reports it as LINEAR): every consumer that trusts the modifier — GL, the
  // display, the CPU — reads scrambled pixels. A VK_IMAGE_TILING_LINEAR image
  // is laid out linearly, so LINEAR is allocated that way there.
  bool linear_via_linear_tiling{false};
  // VK_EXT_image_drm_format_modifier present. Without it (Mesa PowerVR) every
  // image is VK_IMAGE_TILING_LINEAR and LINEAR is the only layout on offer.
  bool modifier_ext{true};
  vk::Buffer kick_buffer;
  vk::DeviceMemory kick_memory;

#if DRM_CXX_HAS_EGL
  // Blit tier: GL draws the exported image into a scannable surface.
  std::unique_ptr<detail::GlBlit> blit;
#endif

  // Command buffer + fence for a new slot; the image is the tier's business.
  Slot& add_slot();
  // Destroy every slot and the copy-tier staging; back to "no buffer".
  void reset_slots() noexcept;

  // A Vulkan-allocated image exported as a dma-buf: the fd (caller closes),
  // the modifier Vulkan chose, and one ExternalPlaneInfo per memory plane.
  struct Exported {
    int fd{-1};
    std::uint64_t modifier{DRM_FORMAT_MOD_INVALID};
    std::vector<scene::ExternalPlaneInfo> planes;
  };
  drm::expected<Exported, std::error_code> export_image(
      Slot& slot, const std::vector<std::uint64_t>& mods) const;

  // Allocate a display-side buffer and bind `slot`'s image to it (LINEAR).
  // Returns the dma-buf fd (caller closes) and its pitch.
  drm::expected<std::pair<int, std::uint32_t>, std::error_code> import_display_buffer(
      Slot& slot, std::uint32_t fourcc);

  // Record acquire barrier → `record` → resolve kick / copy-out → release
  // barrier into slot.cmd. `shared`: the image changes hands with the display
  // or GL (queue-family transfer); `copy_out`: copy it into copy_buffer.
  void record_frame(Slot& slot, const Recorder& record, bool shared, bool copy_out) const;
  // record_frame + submit + CPU wait. Used by the self-checks.
  void submit_and_wait(Slot& slot, const Recorder& record, bool shared) const;
  // Clear `slot` (shared) and wait. Used by the self-checks.
  void clear_and_wait(Slot& slot, std::array<float, 4> rgba) const;
  // True when a GPU write through slot 0's imported image is visible in its
  // display buffer. Some drivers accept a dma-buf import yet render into
  // private memory; scanning that out would show only the stale buffer.
  bool import_aliases();

  // Ring over the slots' dma-bufs; on_release marks a slot free again.
  [[nodiscard]] drm::expected<std::unique_ptr<scene::ExternalDmaBufRing>, std::error_code>
  make_ring(std::uint32_t fourcc, const std::vector<std::vector<scene::ExternalPlaneInfo>>& planes,
            const std::vector<std::uint64_t>& modifiers) const;

  // Export / import tier slot for the next frame: a free one, else the oldest.
  [[nodiscard]] std::size_t pick_slot() const noexcept;

  drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code> setup_copy(
      std::uint32_t fourcc);

#if DRM_CXX_HAS_EGL
  drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code> setup_blit(
      std::uint32_t fourcc, drm::span<const std::uint64_t> allowed, const Exported& exported);
  // True when Vulkan's writes reach the GL surface through the EGLImage.
  bool blit_aliases();
#endif

  ~Impl() {
#if DRM_CXX_HAS_EGL
    blit.reset();  // its EGLImage references this device's memory
#endif
    try {
      if (device) {
        device.waitIdle();
        reset_slots();
        if (export_sem) {
          device.destroySemaphore(export_sem);
        }
        if (kick_buffer) {
          device.destroyBuffer(kick_buffer);
        }
        if (kick_memory) {
          device.freeMemory(kick_memory);
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

VkScanoutProducer::Impl::Slot& VkScanoutProducer::Impl::add_slot() {
  auto slot = std::make_shared<Slot>();
  slot->cmd = device
                  .allocateCommandBuffers(
                      vk::CommandBufferAllocateInfo{cmd_pool, vk::CommandBufferLevel::ePrimary, 1})
                  .front();
  slot->fence = device.createFence(vk::FenceCreateInfo{vk::FenceCreateFlagBits::eSignaled});
  slots.push_back(std::move(slot));
  return *slots.back();
}

void VkScanoutProducer::Impl::reset_slots() noexcept {
  try {
    for (auto& s : slots) {
      if (s->fence) {
        (void)device.waitForFences(s->fence, VK_TRUE, UINT64_MAX);
        device.destroyFence(s->fence);
      }
      if (s->cmd) {
        device.freeCommandBuffers(cmd_pool, s->cmd);
      }
      if (s->image) {
        device.destroyImage(s->image);
      }
      if (s->memory) {
        device.freeMemory(s->memory);
      }
    }
    if (copy_mapped != nullptr) {
      device.unmapMemory(copy_memory);
      copy_mapped = nullptr;
    }
    if (copy_buffer) {
      device.destroyBuffer(copy_buffer);
      copy_buffer = nullptr;
    }
    if (copy_memory) {
      device.freeMemory(copy_memory);
      copy_memory = nullptr;
    }
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer: releasing scanout images: {}", e.what());
  }
  slots.clear();  // also drops the import tier's display buffers
  last_slot = 0;
}

drm::expected<VkScanoutProducer::Impl::Exported, std::error_code>
VkScanoutProducer::Impl::export_image(Slot& slot, const std::vector<std::uint64_t>& mods) const {
  Exported out;
  const bool linear_tiling =
      linear_via_linear_tiling &&
      std::find(mods.begin(), mods.end(), DRM_FORMAT_MOD_LINEAR) != mods.end() &&
      static_cast<bool>(physical.getFormatProperties(format).linearTilingFeatures &
                        vk::FormatFeatureFlagBits::eColorAttachment);
  if (!modifier_ext && !linear_tiling) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  try {
    if (linear_tiling) {
      vk::StructureChain<vk::ImageCreateInfo, vk::ExternalMemoryImageCreateInfo> image_chain{
          vk::ImageCreateInfo{}
              .setImageType(vk::ImageType::e2D)
              .setFormat(format)
              .setExtent({extent.width, extent.height, 1})
              .setMipLevels(1)
              .setArrayLayers(1)
              .setSamples(vk::SampleCountFlagBits::e1)
              .setTiling(vk::ImageTiling::eLinear)
              .setUsage(k_image_usage)
              .setSharingMode(vk::SharingMode::eExclusive)
              .setInitialLayout(vk::ImageLayout::eUndefined),
          vk::ExternalMemoryImageCreateInfo{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT}};
      slot.image = device.createImage(image_chain.get<vk::ImageCreateInfo>());
    } else {
      vk::StructureChain<vk::ImageCreateInfo, vk::ExternalMemoryImageCreateInfo,
                         vk::ImageDrmFormatModifierListCreateInfoEXT>
          image_chain{
              vk::ImageCreateInfo{}
                  .setImageType(vk::ImageType::e2D)
                  .setFormat(format)
                  .setExtent({extent.width, extent.height, 1})
                  .setMipLevels(1)
                  .setArrayLayers(1)
                  .setSamples(vk::SampleCountFlagBits::e1)
                  .setTiling(vk::ImageTiling::eDrmFormatModifierEXT)
                  .setUsage(k_image_usage)
                  .setSharingMode(vk::SharingMode::eExclusive)
                  .setInitialLayout(vk::ImageLayout::eUndefined),
              vk::ExternalMemoryImageCreateInfo{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT},
              vk::ImageDrmFormatModifierListCreateInfoEXT{}.setDrmFormatModifiers(mods)};
      slot.image = device.createImage(image_chain.get<vk::ImageCreateInfo>());
    }

    const vk::MemoryRequirements mr = device.getImageMemoryRequirements(slot.image);
    const std::uint32_t type_index =
        find_memory_type(physical, mr.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    if (type_index == UINT32_MAX) {
      return drm::unexpected<std::error_code>(err(std::errc::not_supported));
    }
    vk::StructureChain<vk::MemoryAllocateInfo, vk::ExportMemoryAllocateInfo> alloc_chain{
        vk::MemoryAllocateInfo{mr.size, type_index},
        vk::ExportMemoryAllocateInfo{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT}};
    slot.memory = device.allocateMemory(alloc_chain.get<vk::MemoryAllocateInfo>());
    device.bindImageMemory(slot.image, slot.memory, 0);

    out.fd = device.getMemoryFdKHR(
        vk::MemoryGetFdInfoKHR{slot.memory, vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT});

    if (linear_tiling) {
      const vk::SubresourceLayout layout = device.getImageSubresourceLayout(
          slot.image, vk::ImageSubresource{vk::ImageAspectFlagBits::eColor, 0, 0});
      out.modifier = DRM_FORMAT_MOD_LINEAR;
      out.planes.push_back(scene::ExternalPlaneInfo{out.fd,
                                                    static_cast<std::uint32_t>(layout.offset),
                                                    static_cast<std::uint32_t>(layout.rowPitch)});
      return out;
    }

    // The actual modifier Vulkan assigned, and its plane count.
    out.modifier = device.getImageDrmFormatModifierPropertiesEXT(slot.image).drmFormatModifier;
    std::uint32_t plane_count = 1;
    {
      vk::DrmFormatModifierPropertiesListEXT list;
      vk::FormatProperties2 fp;
      fp.pNext = &list;
      physical.getFormatProperties2(format, &fp);
      std::vector<vk::DrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
      list.pDrmFormatModifierProperties = props.data();
      physical.getFormatProperties2(format, &fp);
      for (const auto& mod : props) {
        if (mod.drmFormatModifier == out.modifier) {
          plane_count = mod.drmFormatModifierPlaneCount;
          break;
        }
      }
    }

    for (std::uint32_t p = 0; (p < plane_count) && (p < memory_planes.size()); ++p) {
      const vk::SubresourceLayout layout =
          device.getImageSubresourceLayout(slot.image, vk::ImageSubresource{memory_planes.at(p)});
      out.planes.push_back(scene::ExternalPlaneInfo{out.fd,
                                                    static_cast<std::uint32_t>(layout.offset),
                                                    static_cast<std::uint32_t>(layout.rowPitch)});
    }
  } catch (const std::exception& e) {
    if (out.fd >= 0) {
      ::close(out.fd);
    }
    drm::log_warn("VkScanoutProducer::create_buffer: {}", e.what());
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  return out;
}

drm::expected<std::pair<int, std::uint32_t>, std::error_code>
VkScanoutProducer::Impl::import_display_buffer(Slot& slot, std::uint32_t fourcc) {
  auto alloc = alloc_display(*dev, gbm_dev, extent.width, extent.height, fourcc);
  if (!alloc) {
    return drm::unexpected<std::error_code>(err(std::errc::not_enough_memory));
  }
  try {
    auto make_image = [&](std::uint32_t pitch) {
      if (!modifier_ext) {
        // No explicit-layout create: take a LINEAR-tiled image and use it only
        // if the driver picked the display buffer's pitch.
        vk::StructureChain<vk::ImageCreateInfo, vk::ExternalMemoryImageCreateInfo> linear_chain{
            vk::ImageCreateInfo{}
                .setImageType(vk::ImageType::e2D)
                .setFormat(format)
                .setExtent({extent.width, extent.height, 1})
                .setMipLevels(1)
                .setArrayLayers(1)
                .setSamples(vk::SampleCountFlagBits::e1)
                .setTiling(vk::ImageTiling::eLinear)
                .setUsage(k_image_usage)
                .setSharingMode(vk::SharingMode::eExclusive)
                .setInitialLayout(vk::ImageLayout::eUndefined),
            vk::ExternalMemoryImageCreateInfo{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT}};
        const vk::Image image = device.createImage(linear_chain.get<vk::ImageCreateInfo>());
        const vk::SubresourceLayout layout = device.getImageSubresourceLayout(
            image, vk::ImageSubresource{vk::ImageAspectFlagBits::eColor, 0, 0});
        if (layout.offset != 0 || layout.rowPitch != pitch) {
          device.destroyImage(image);
          throw std::runtime_error("linear image pitch does not match the display buffer");
        }
        return image;
      }
      const vk::SubresourceLayout plane_layout{0, 0, pitch, 0, 0};
      vk::StructureChain<vk::ImageCreateInfo, vk::ExternalMemoryImageCreateInfo,
                         vk::ImageDrmFormatModifierExplicitCreateInfoEXT>
          image_chain{
              vk::ImageCreateInfo{}
                  .setImageType(vk::ImageType::e2D)
                  .setFormat(format)
                  .setExtent({extent.width, extent.height, 1})
                  .setMipLevels(1)
                  .setArrayLayers(1)
                  .setSamples(vk::SampleCountFlagBits::e1)
                  .setTiling(vk::ImageTiling::eDrmFormatModifierEXT)
                  .setUsage(k_image_usage)
                  .setSharingMode(vk::SharingMode::eExclusive)
                  .setInitialLayout(vk::ImageLayout::eUndefined),
              vk::ExternalMemoryImageCreateInfo{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT},
              vk::ImageDrmFormatModifierExplicitCreateInfoEXT{}
                  .setDrmFormatModifier(DRM_FORMAT_MOD_LINEAR)
                  .setPlaneLayouts(plane_layout)};
      return device.createImage(image_chain.get<vk::ImageCreateInfo>());
    };
    slot.image = make_image(alloc->pitch);
    vk::MemoryRequirements mr = device.getImageMemoryRequirements(slot.image);
    // GPUs commonly pad an image's height (Vivante: to 16 rows), so the image
    // can need more bytes than a width x height buffer holds. Reallocate with
    // enough extra rows at the same pitch; the framebuffer still covers only
    // width x height, the padding rows are never scanned out.
    if (mr.size > alloc->size && alloc->pitch != 0U) {
      const auto rows = static_cast<std::uint32_t>((mr.size + alloc->pitch - 1U) /
                                                   static_cast<vk::DeviceSize>(alloc->pitch));
      auto padded = alloc_display(*dev, gbm_dev, extent.width, rows, fourcc);
      if (!padded || padded->pitch != alloc->pitch) {
        throw std::runtime_error("could not allocate a padded display buffer");
      }
      ::close(alloc->fd);
      alloc = std::move(padded);
    }
    const vk::MemoryFdPropertiesKHR fd_props = device.getMemoryFdPropertiesKHR(
        vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, alloc->fd);
    const std::uint32_t type_bits = mr.memoryTypeBits & fd_props.memoryTypeBits;
    std::uint32_t type_index = UINT32_MAX;
    for (std::uint32_t i = 0; i < 32U; ++i) {
      if ((type_bits & (1U << i)) != 0U) {
        type_index = i;
        break;
      }
    }
    if (type_index == UINT32_MAX || mr.size > alloc->size) {
      drm::log_warn(
          "VkScanoutProducer: image needs {} bytes (types 0x{:x}); display buffer is {} bytes "
          "(fd types 0x{:x}), pitch {}",
          static_cast<std::uint64_t>(mr.size), mr.memoryTypeBits,
          static_cast<std::uint64_t>(alloc->size), fd_props.memoryTypeBits, alloc->pitch);
      throw std::runtime_error("display buffer is not importable for this image");
    }
    // Vulkan owns the fd it imports; keep ours for the scene's PRIME import.
    const int vk_fd = ::fcntl(alloc->fd, F_DUPFD_CLOEXEC, 0);
    if (vk_fd < 0) {
      throw std::runtime_error("dup of the display buffer fd failed");
    }
    vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportMemoryFdInfoKHR,
                       vk::MemoryDedicatedAllocateInfo>
        alloc_chain{
            vk::MemoryAllocateInfo{mr.size, type_index},
            vk::ImportMemoryFdInfoKHR{vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, vk_fd},
            vk::MemoryDedicatedAllocateInfo{slot.image, nullptr}};
    try {
      slot.memory = device.allocateMemory(alloc_chain.get<vk::MemoryAllocateInfo>());
    } catch (...) {
      ::close(vk_fd);  // not consumed on failure
      throw;
    }
    device.bindImageMemory(slot.image, slot.memory, 0);
  } catch (const std::exception& e) {
    ::close(alloc->fd);
    drm::log_warn("VkScanoutProducer: display-side import failed: {}", e.what());
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  slot.display_gbm = std::move(alloc->gbm);
  slot.display_dumb = std::move(alloc->dumb);
  return std::make_pair(alloc->fd, alloc->pitch);
}

void VkScanoutProducer::Impl::record_frame(Slot& slot, const Recorder& record, bool shared,
                                           bool copy_out) const {
  const vk::CommandBuffer cmd = slot.cmd;
  cmd.reset();
  cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

  constexpr vk::AccessFlags k_writes =
      vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eTransferWrite;
  constexpr vk::AccessFlags k_any = k_writes | vk::AccessFlagBits::eColorAttachmentRead |
                                    vk::AccessFlagBits::eTransferRead |
                                    vk::AccessFlagBits::eShaderRead;
  // Acquire from the display (or GL), or initialize a fresh image. Its
  // contents are not preserved for a fresh image; otherwise they are.
  const bool transfer = shared && !slot.fresh;
  cmd.pipelineBarrier(
      vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
      vk::ImageMemoryBarrier{{},
                             k_any,
                             slot.fresh ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
                             vk::ImageLayout::eGeneral,
                             transfer ? foreign_family : VK_QUEUE_FAMILY_IGNORED,
                             transfer ? queue_family : VK_QUEUE_FAMILY_IGNORED,
                             slot.image,
                             k_color_range});

  record(static_cast<VkCommandBuffer>(cmd), static_cast<VkImage>(slot.image));

  const bool kick = shared && resolve_kick;
  if (kick || copy_out) {
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                        vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                        vk::ImageMemoryBarrier{k_writes, vk::AccessFlagBits::eTransferRead,
                                               vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                               slot.image, k_color_range});
  }
  if (kick) {
    cmd.copyImageToBuffer(
        slot.image, vk::ImageLayout::eGeneral, kick_buffer,
        vk::BufferImageCopy{
            0, 0, 0, {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}});
  }
  if (copy_out) {
    cmd.copyImageToBuffer(slot.image, vk::ImageLayout::eGeneral, copy_buffer,
                          vk::BufferImageCopy{0,
                                              0,
                                              0,
                                              {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                              {0, 0, 0},
                                              {extent.width, extent.height, 1}});
    cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {},
        vk::BufferMemoryBarrier{vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
                                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, copy_buffer, 0,
                                VK_WHOLE_SIZE},
        {});
  }
  if (shared) {
    // Release to the display (or GL). Without this a GPU may keep the result
    // only in its own compression / fast-clear metadata and the consumer reads
    // the untouched memory underneath — a black screen.
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                        vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {},
                        vk::ImageMemoryBarrier{k_writes | vk::AccessFlagBits::eTransferRead,
                                               {},
                                               vk::ImageLayout::eGeneral,
                                               vk::ImageLayout::eGeneral,
                                               queue_family,
                                               foreign_family,
                                               slot.image,
                                               k_color_range});
  }
  cmd.end();
}

void VkScanoutProducer::Impl::submit_and_wait(Slot& slot, const Recorder& record,
                                              bool shared) const {
  (void)device.waitForFences(slot.fence, VK_TRUE, UINT64_MAX);
  device.resetFences(slot.fence);
  record_frame(slot, record, shared, false);
  queue.submit(vk::SubmitInfo{}.setCommandBuffers(slot.cmd), slot.fence);
  (void)device.waitForFences(slot.fence, VK_TRUE, UINT64_MAX);
  slot.fresh = false;
}

void VkScanoutProducer::Impl::clear_and_wait(Slot& slot, std::array<float, 4> rgba) const {
  submit_and_wait(
      slot,
      [&rgba](void* cmd, void* image) {
        vk::CommandBuffer(static_cast<VkCommandBuffer>(cmd))
            .clearColorImage(
                vk::Image(static_cast<VkImage>(image)), vk::ImageLayout::eGeneral,
                vk::ClearColorValue{std::array<float, 4>{rgba[0], rgba[1], rgba[2], rgba[3]}},
                k_color_range);
      },
      true);
}

bool VkScanoutProducer::Impl::import_aliases() {
  if (slots.empty()) {
    return false;
  }
  Slot& slot = *slots.front();
  constexpr std::uint32_t k_marker = 0x01020304U;
  auto cpu_pixel = [&slot](std::optional<std::uint32_t> write) -> std::optional<std::uint32_t> {
    if (!slot.display_gbm.empty()) {
      auto m = slot.display_gbm.map(write ? drm::MapAccess::Write : drm::MapAccess::Read);
      if (!m || m->pixels().size() < sizeof(std::uint32_t)) {
        return std::nullopt;
      }
      std::uint32_t v = 0;
      if (write) {
        std::memcpy(m->pixels().data(), &*write, sizeof v);
      }
      std::memcpy(&v, m->pixels().data(), sizeof v);
      return v;
    }
    if (!slot.display_dumb.empty() && slot.display_dumb.data() != nullptr) {
      std::uint32_t v = 0;
      if (write) {
        std::memcpy(slot.display_dumb.data(), &*write, sizeof v);
      }
      std::memcpy(&v, slot.display_dumb.data(), sizeof v);
      return v;
    }
    return std::nullopt;
  };
  if (!cpu_pixel(k_marker).has_value()) {
    return false;
  }
  try {
    clear_and_wait(slot, {0.0F, 1.0F, 0.0F, 1.0F});
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer: import self-check failed: {}", e.what());
    return false;
  }
  const auto after = cpu_pixel(std::nullopt);
  return after.has_value() && *after != k_marker;
}

drm::expected<std::unique_ptr<scene::ExternalDmaBufRing>, std::error_code>
VkScanoutProducer::Impl::make_ring(std::uint32_t fourcc,
                                   const std::vector<std::vector<scene::ExternalPlaneInfo>>& planes,
                                   const std::vector<std::uint64_t>& modifiers) const {
  std::vector<scene::ExternalSlotDesc> descs;
  descs.reserve(planes.size());
  for (std::size_t i = 0; i < planes.size(); ++i) {
    descs.push_back(scene::ExternalSlotDesc{
        modifiers.at(i),
        drm::span<const scene::ExternalPlaneInfo>(planes.at(i).data(), planes.at(i).size())});
  }
  scene::ExternalDmaBufRing::Options opts;
  opts.on_release = [held = slots](std::size_t index, std::optional<drm::sync::SyncFence> fence) {
    if (index >= held.size()) {
      return;
    }
    Slot& s = *held.at(index);
    {
      const std::lock_guard<std::mutex> lock(s.mu);
      s.release_fence = std::move(fence);
    }
    s.free = true;
  };
  return scene::ExternalDmaBufRing::create(
      *dev, extent.width, extent.height, fourcc,
      drm::span<const scene::ExternalSlotDesc>(descs.data(), descs.size()), std::move(opts));
}

std::size_t VkScanoutProducer::Impl::pick_slot() const noexcept {
  const std::size_t n = slots.size();
  if (n == 0) {
    return 0;
  }
  for (std::size_t k = 1; k <= n; ++k) {
    const std::size_t i = (last_slot + k) % n;
    if (slots.at(i)->free) {
      return i;
    }
  }
  // Every slot is held: frames are outrunning the flips. The oldest submit is
  // the one furthest from the screen (the scene keeps its latest on screen).
  return (last_slot + 1) % n;
}

drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
VkScanoutProducer::Impl::setup_copy(std::uint32_t fourcc) {
  try {
    Slot& slot = add_slot();
    slot.image = device.createImage(vk::ImageCreateInfo{}
                                        .setImageType(vk::ImageType::e2D)
                                        .setFormat(format)
                                        .setExtent({extent.width, extent.height, 1})
                                        .setMipLevels(1)
                                        .setArrayLayers(1)
                                        .setSamples(vk::SampleCountFlagBits::e1)
                                        .setTiling(vk::ImageTiling::eOptimal)
                                        .setUsage(k_image_usage)
                                        .setSharingMode(vk::SharingMode::eExclusive)
                                        .setInitialLayout(vk::ImageLayout::eUndefined));
    const vk::MemoryRequirements imr = device.getImageMemoryRequirements(slot.image);
    const std::uint32_t image_type =
        find_memory_type(physical, imr.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    if (image_type == UINT32_MAX) {
      throw std::runtime_error("no device-local memory for the render target");
    }
    slot.memory = device.allocateMemory(vk::MemoryAllocateInfo{imr.size, image_type});
    device.bindImageMemory(slot.image, slot.memory, 0);

    const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(extent.width) * extent.height * 4U;
    copy_buffer =
        device.createBuffer(vk::BufferCreateInfo{{}, bytes, vk::BufferUsageFlagBits::eTransferDst});
    const vk::MemoryRequirements bmr = device.getBufferMemoryRequirements(copy_buffer);
    std::uint32_t buffer_type = find_memory_type(
        physical, bmr.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached);
    copy_coherent = false;
    if (buffer_type == UINT32_MAX) {
      buffer_type =
          find_memory_type(physical, bmr.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible);
      copy_coherent = true;  // uncached host memory is coherent by spec
    }
    if (buffer_type == UINT32_MAX) {
      throw std::runtime_error("no host-visible memory for the copy-out buffer");
    }
    if (!copy_coherent) {
      const vk::PhysicalDeviceMemoryProperties props = physical.getMemoryProperties();
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      copy_coherent = static_cast<bool>(props.memoryTypes[buffer_type].propertyFlags &
                                        vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    copy_memory = device.allocateMemory(vk::MemoryAllocateInfo{bmr.size, buffer_type});
    device.bindBufferMemory(copy_buffer, copy_memory, 0);
    copy_mapped = device.mapMemory(copy_memory, 0, VK_WHOLE_SIZE);
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer: copy-path render target: {}", e.what());
    reset_slots();
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  std::array<int, 2> fds{-1, -1};
  std::array<std::array<scene::ExternalPlaneInfo, 1>, 2> planes{};
  std::array<scene::ExternalSlotDesc, 2> descs{};
  auto close_fds = [&fds] {
    for (int& f : fds) {
      if (f >= 0) {
        ::close(f);
        f = -1;
      }
    }
  };
  for (std::size_t i = 0; i < 2; ++i) {
    auto buf =
        dumb::Buffer::create(*dev, dumb::Config{extent.width, extent.height, fourcc, 32, false});
    if (!buf ||
        drmPrimeHandleToFD(dev->fd(), buf->handle(), DRM_CLOEXEC | DRM_RDWR, &fds.at(i)) != 0) {
      close_fds();
      reset_slots();
      return drm::unexpected<std::error_code>(buf ? err(std::errc::io_error) : buf.error());
    }
    planes.at(i).at(0) = scene::ExternalPlaneInfo{fds.at(i), 0, buf->stride()};
    descs.at(i) = scene::ExternalSlotDesc{
        DRM_FORMAT_MOD_LINEAR,
        drm::span<const scene::ExternalPlaneInfo>(planes.at(i).data(), planes.at(i).size())};
    copy_buffers.at(i) = std::move(*buf);
    copy_slot_free->at(i) = true;
  }
  scene::ExternalDmaBufRing::Options opts;
  opts.on_release = [flags = copy_slot_free](std::size_t index,
                                             std::optional<drm::sync::SyncFence> /*fence*/) {
    if (index < flags->size()) {
      flags->at(index) = true;
    }
  };
  auto ring_r = scene::ExternalDmaBufRing::create(
      *dev, extent.width, extent.height, fourcc,
      drm::span<const scene::ExternalSlotDesc>(descs.data(), descs.size()), std::move(opts));
  close_fds();  // the ring dups them
  if (!ring_r) {
    reset_slots();
    return drm::unexpected<std::error_code>(ring_r.error());
  }
  ring = ring_r->get();
  mode = Mode::Copy;
  return std::unique_ptr<scene::LayerBufferSource>(std::move(*ring_r));
}

#if DRM_CXX_HAS_EGL

drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
VkScanoutProducer::Impl::setup_blit(std::uint32_t fourcc, drm::span<const std::uint64_t> allowed,
                                    const Exported& exported) {
  if (exported.planes.size() != 1) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  const scene::ExternalPlaneInfo& plane = exported.planes.front();
  auto created = detail::GlBlit::create(
      *dev, extent.width, extent.height, fourcc, allowed,
      detail::GlBlitInput{exported.fd, plane.offset, plane.pitch, exported.modifier});
  if (!created) {
    return drm::unexpected<std::error_code>(created.error());
  }
  blit = std::move(*created);
  if (!blit_aliases()) {
    drm::log_info("VkScanoutProducer: GL does not see Vulkan's writes through the EGLImage");
    blit.reset();
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  mode = Mode::Blit;
  return blit->take_source();
}

bool VkScanoutProducer::Impl::blit_aliases() {
  // Two distinct colors, so a buffer that happened to hold the first cannot pass.
  const std::array<std::array<float, 4>, 2> colors{
      {{0.0F, 1.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}}};
  for (const auto& c : colors) {
    try {
      clear_and_wait(*slots.front(), c);
    } catch (const std::exception& e) {
      drm::log_warn("VkScanoutProducer: blit self-check failed: {}", e.what());
      return false;
    }
    if (!blit->draw()) {
      return false;
    }
    const auto px = blit->read_center();
    if (!px) {
      return true;  // no glReadPixels to check with; trust the import
    }
    const bool red = c[0] > 0.5F;
    if (((*px)[0] > 128U) != red || ((*px)[1] > 128U) == red) {
      drm::log_info(
          "VkScanoutProducer: blit self-check read {:02x} {:02x} {:02x} {:02x} (expected {})",
          (*px)[0], (*px)[1], (*px)[2], (*px)[3], red ? "red" : "green");
      return false;
    }
  }
  return true;
}

#endif  // DRM_CXX_HAS_EGL

VkScanoutProducer::VkScanoutProducer() = default;
VkScanoutProducer::~VkScanoutProducer() = default;

drm::expected<std::unique_ptr<VkScanoutProducer>, std::error_code> VkScanoutProducer::create(
    drm::Device& dev) {
  return create(dev, Options{});
}

drm::expected<std::unique_ptr<VkScanoutProducer>, std::error_code> VkScanoutProducer::create(
    drm::Device& dev, const Options& options) {
  auto impl = std::make_unique<Impl>();
  impl->dev = &dev;
  impl->buffer_count = std::max<std::uint32_t>(1U, options.buffer_count);

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
      // KMS device as a dmabuf (the scene imports it on impl->dev).
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
    // VK_KHR_external_memory / _semaphore are core since 1.1 (the instance asks
    // for 1.1), and a 1.1+ driver may leave them off its extension list
    // (VeriSilicon/Vivante does) — enabling an unlisted name fails createDevice
    // with ErrorExtensionNotPresent. Request those two only when listed; the
    // rest have no core equivalent and stay mandatory.
    const auto listed = impl->physical.enumerateDeviceExtensionProperties();
    const auto is_listed = [&listed](std::string_view name) {
      return std::any_of(listed.begin(), listed.end(), [name](const vk::ExtensionProperties& e) {
        return std::string_view(e.extensionName.data()) == name;
      });
    };
    const bool core_1_1 = impl->physical.getProperties().apiVersion >= VK_API_VERSION_1_1;
    std::vector<const char*> dev_exts{"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
                                      "VK_KHR_external_semaphore_fd"};
    // Without the modifier extension (Mesa PowerVR) images fall back to
    // VK_IMAGE_TILING_LINEAR, exported as LINEAR.
    if (is_listed("VK_EXT_image_drm_format_modifier")) {
      dev_exts.push_back("VK_EXT_image_drm_format_modifier");
    } else {
      impl->modifier_ext = false;
      impl->linear_via_linear_tiling = true;
      drm::log_info(
          "VkScanoutProducer: no VK_EXT_image_drm_format_modifier; LINEAR via linear tiling");
    }
    // VK_EXT_image_drm_format_modifier requires VK_KHR_image_format_list on a
    // 1.1 device (core in 1.2); enable it whenever it is listed.
    if (is_listed("VK_KHR_image_format_list")) {
      dev_exts.push_back("VK_KHR_image_format_list");
    }
    for (const char* promoted : {"VK_KHR_external_memory", "VK_KHR_external_semaphore"}) {
      if (!core_1_1 || is_listed(promoted)) {
        dev_exts.push_back(promoted);
      }
    }
    // FOREIGN names "a different device / the display" more precisely than
    // EXTERNAL; use it when offered.
    if (is_listed("VK_EXT_queue_family_foreign")) {
      dev_exts.push_back("VK_EXT_queue_family_foreign");
      impl->foreign_family = VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
    impl->device = impl->physical.createDevice(
        vk::DeviceCreateInfo{}.setQueueCreateInfos(qci).setPEnabledExtensionNames(dev_exts));
    VULKAN_HPP_DEFAULT_DISPATCHER.init(impl->device);
    impl->queue = impl->device.getQueue(qf, 0);

    impl->cmd_pool = impl->device.createCommandPool(
        vk::CommandPoolCreateInfo{vk::CommandPoolCreateFlagBits::eResetCommandBuffer, qf});

    // A semaphore signaled by each render submit and exported as a sync_file
    // (the acquire fence KMS waits on).
    vk::StructureChain<vk::SemaphoreCreateInfo, vk::ExportSemaphoreCreateInfo> sem_chain{
        vk::SemaphoreCreateInfo{},
        vk::ExportSemaphoreCreateInfo{vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd}};
    impl->export_sem = impl->device.createSemaphore(sem_chain.get<vk::SemaphoreCreateInfo>());

    if (impl->physical.getProperties().vendorID == VK_VENDOR_ID_VSI) {
      impl->linear_via_linear_tiling = true;
      // Best effort: without the kick, cleared-only pixels may show stale.
      try {
        impl->kick_buffer = impl->device.createBuffer(
            vk::BufferCreateInfo{{}, 4, vk::BufferUsageFlagBits::eTransferDst});
        const vk::MemoryRequirements mr =
            impl->device.getBufferMemoryRequirements(impl->kick_buffer);
        std::uint32_t type_index = 0;
        while (type_index < 32U && (mr.memoryTypeBits & (1U << type_index)) == 0U) {
          ++type_index;
        }
        if (type_index < 32U) {
          impl->kick_memory =
              impl->device.allocateMemory(vk::MemoryAllocateInfo{mr.size, type_index});
          impl->device.bindBufferMemory(impl->kick_buffer, impl->kick_memory, 0);
          impl->resolve_kick = true;
        }
      } catch (const std::exception& e) {
        drm::log_warn("VkScanoutProducer: tile-status resolve buffer: {}", e.what());
      }
    }
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
  if (!impl_->modifier_ext) {
    // LINEAR via linear tiling, when the format renders that way.
    if (impl_->physical.getFormatProperties(format).linearTilingFeatures &
        vk::FormatFeatureFlagBits::eColorAttachment) {
      out.push_back(DRM_FORMAT_MOD_LINEAR);
    }
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
  Impl& p = *impl_;
  if (!p.slots.empty()) {
    return drm::unexpected<std::error_code>(err(std::errc::already_connected));
  }
  p.format = vk_format_for(fourcc);
  if (p.format == vk::Format::eUndefined) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  p.extent = vk::Extent2D{width, height};

  // The backend hands us the negotiated set (Vulkan-renderable ∩ plane); offer
  // it (or LINEAR) to the modifier-list image-create. Vulkan picks one.
  std::vector<std::uint64_t> mods(allowed.begin(), allowed.end());
  if (mods.empty()) {
    mods.push_back(DRM_FORMAT_MOD_LINEAR);
  }

  // Export tier: buffer_count Vulkan-allocated images the display imports.
  std::error_code export_error = err(std::errc::io_error);
  try {
    std::vector<std::vector<scene::ExternalPlaneInfo>> planes;
    std::vector<std::uint64_t> modifiers;
    std::vector<int> fds;
    bool ok = true;
    for (std::uint32_t i = 0; i < p.buffer_count; ++i) {
      auto exported = p.export_image(p.add_slot(), mods);
      if (!exported) {
        export_error = exported.error();
        ok = false;
        break;
      }
      fds.push_back(exported->fd);
      planes.push_back(exported->planes);
      modifiers.push_back(exported->modifier);
    }
    if (ok) {
      auto ring = p.make_ring(fourcc, planes, modifiers);
      for (const int fd : fds) {
        ::close(fd);  // the ring dups them
      }
      if (ring) {
        p.ring = ring->get();
        p.mode = Impl::Mode::Export;
        return std::unique_ptr<scene::LayerBufferSource>(std::move(*ring));
      }
      export_error = ring.error();
    } else {
      for (const int fd : fds) {
        ::close(fd);
      }
    }
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer::create_buffer: {}", e.what());
  }
  p.reset_slots();

  // The display could not import Vulkan's buffer. Typical cause: a display
  // controller without an IOMMU (CMA / GEM-DMA, e.g. i.MX LCDIF) importing a
  // scatter-gather GPU allocation — the PRIME import has to bounce it and
  // fails (ENOMEM). Fall through the remaining tiers, best first:
  //   1. reverse the direction — the display allocates (memory it can scan
  //      out) and Vulkan imports it as LINEAR, kept only if a GPU write
  //      through the import is actually visible;
  //   2. GPU blit — the GPU's GL stack imports Vulkan's buffer and draws it
  //      into a scannable surface each frame, kept only if GL sees the writes;
  //   3. CPU copy into display-side dumb buffers.
  drm::log_info("VkScanoutProducer: display could not import the Vulkan buffer ({})",
                export_error.message());

  const auto renderable = exportable_modifiers(fourcc);
  if (std::find(renderable.begin(), renderable.end(), DRM_FORMAT_MOD_LINEAR) != renderable.end()) {
    try {
      std::vector<std::vector<scene::ExternalPlaneInfo>> planes;
      std::vector<int> fds;
      bool ok = true;
      for (std::uint32_t i = 0; i < p.buffer_count; ++i) {
        auto imported = p.import_display_buffer(p.add_slot(), fourcc);
        if (!imported) {
          ok = false;
          break;
        }
        fds.push_back(imported->first);
        planes.push_back({scene::ExternalPlaneInfo{imported->first, 0, imported->second}});
        // Self-check on the first: an import that does not alias would scan
        // out a stale buffer, so stop at once.
        if (i == 0 && !p.import_aliases()) {
          drm::log_info("VkScanoutProducer: the Vulkan driver does not alias imported dma-bufs");
          ok = false;
          break;
        }
      }
      if (ok) {
        auto ring = p.make_ring(fourcc, planes,
                                std::vector<std::uint64_t>(planes.size(), DRM_FORMAT_MOD_LINEAR));
        for (const int fd : fds) {
          ::close(fd);
        }
        if (ring) {
          drm::log_info(
              "VkScanoutProducer: zero-copy via display-side buffers imported into Vulkan");
          p.ring = ring->get();
          p.mode = Impl::Mode::Import;
          return std::unique_ptr<scene::LayerBufferSource>(std::move(*ring));
        }
      } else {
        for (const int fd : fds) {
          ::close(fd);
        }
      }
    } catch (const std::exception& e) {
      drm::log_warn("VkScanoutProducer: display-side import: {}", e.what());
    }
    p.reset_slots();
  }

#if DRM_CXX_HAS_EGL
  try {
    if (auto exported = p.export_image(p.add_slot(), mods); exported) {
      auto blit = p.setup_blit(fourcc, allowed, *exported);
      ::close(exported->fd);  // the EGLImage holds its own reference
      if (blit) {
        drm::log_info(
            "VkScanoutProducer: each frame is copied on the GPU (GL draw of the Vulkan buffer) "
            "into a scannable surface");
        return blit;
      }
    }
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer: GPU-copy tier: {}", e.what());
  }
  p.reset_slots();
#endif

  auto copied = p.setup_copy(fourcc);
  if (!copied) {
    return drm::unexpected<std::error_code>(copied.error());
  }
  drm::log_info(
      "VkScanoutProducer: no zero-copy or GPU-copy path between this GPU and display; each frame "
      "is copied by the CPU into a display-side buffer");
  return copied;
}

drm::expected<void, std::error_code> VkScanoutProducer::render(const Recorder& record) {
  Impl& p = *impl_;
  if (p.slots.empty() || !record) {
    return drm::unexpected<std::error_code>(err(std::errc::not_connected));
  }
  try {
    const bool ring_tier = (p.mode == Impl::Mode::Export) || (p.mode == Impl::Mode::Import);
    const std::size_t index = ring_tier ? p.pick_slot() : 0U;
    Impl::Slot& slot = *p.slots.at(index);

    // The slot's previous submit, then the display's release of it.
    (void)p.device.waitForFences(slot.fence, VK_TRUE, UINT64_MAX);
    p.device.resetFences(slot.fence);
    std::optional<drm::sync::SyncFence> release;
    {
      const std::lock_guard<std::mutex> lock(slot.mu);
      release.swap(slot.release_fence);
    }
    if (release.has_value()) {
      if (auto r = release->wait(std::chrono::seconds(1)); !r) {
        drm::log_warn("VkScanoutProducer: release-fence wait: {}", r.error().message());
      }
    }

    const bool copy = p.mode == Impl::Mode::Copy;
    p.record_frame(slot, record, !copy, copy);

    if (copy) {
      p.queue.submit(vk::SubmitInfo{}.setCommandBuffers(slot.cmd), slot.fence);
      (void)p.device.waitForFences(slot.fence, VK_TRUE, UINT64_MAX);
      slot.fresh = false;
      if (!p.copy_coherent) {
        p.device.invalidateMappedMemoryRanges(
            vk::MappedMemoryRange{p.copy_memory, 0, VK_WHOLE_SIZE});
      }
      // Write the dumb buffer the display is not holding; both held only if
      // frames outrun the flips, in which case the pending one is overwritten.
      std::size_t target = p.copy_next;
      if (!p.copy_slot_free->at(target) && p.copy_slot_free->at(target ^ 1U)) {
        target ^= 1U;
      }
      dumb::Buffer& dst = p.copy_buffers.at(target);
      const auto* src = static_cast<const std::uint8_t*>(p.copy_mapped);
      const std::size_t row_bytes = static_cast<std::size_t>(p.extent.width) * 4U;
      for (std::uint32_t y = 0; y < p.extent.height; ++y) {
        std::memcpy(dst.data() + (static_cast<std::size_t>(y) * dst.stride()),
                    src + (static_cast<std::size_t>(y) * row_bytes), row_bytes);
      }
      p.copy_slot_free->at(target) = false;
      p.ring->submit(target);
      p.copy_next = target ^ 1U;
      return {};
    }

#if DRM_CXX_HAS_EGL
    if (p.mode == Impl::Mode::Blit) {
      // GL reads the image next, so wait for the render here; the GL draw
      // then carries its own native fence to KMS via swap_buffers().
      p.queue.submit(vk::SubmitInfo{}.setCommandBuffers(slot.cmd), slot.fence);
      (void)p.device.waitForFences(slot.fence, VK_TRUE, UINT64_MAX);
      slot.fresh = false;
      if (!p.blit->draw() || !p.blit->present()) {
        return drm::unexpected<std::error_code>(err(std::errc::io_error));
      }
      return {};
    }
#endif

    // Export / import: submit WITHOUT a CPU wait, signaling the export
    // semaphore (-> the sync_file KMS waits on) and the slot's fence.
    p.queue.submit(vk::SubmitInfo{}.setCommandBuffers(slot.cmd).setSignalSemaphores(p.export_sem),
                   slot.fence);
    slot.fresh = false;
    slot.free = false;
    p.last_slot = index;
    const int sem_fd = p.device.getSemaphoreFdKHR(
        vk::SemaphoreGetFdInfoKHR{p.export_sem, vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd});
    auto fence = drm::sync::SyncFence::import_fd(sem_fd);
    if (sem_fd >= 0) {
      ::close(sem_fd);  // import_fd dups
    }
    if (!fence) {
      drm::log_warn("VkScanoutProducer::render: sync_file import failed: {}",
                    fence.error().message());
      p.ring->submit(index);
    } else {
      p.ring->submit(index, std::move(*fence));
    }
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer::render: {}", e.what());
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  return {};
}

drm::expected<void, std::error_code> VkScanoutProducer::render_clear(std::array<float, 4> rgba) {
  return render([&rgba](void* cmd, void* image) {
    vk::CommandBuffer(static_cast<VkCommandBuffer>(cmd))
        .clearColorImage(
            vk::Image(static_cast<VkImage>(image)), vk::ImageLayout::eGeneral,
            vk::ClearColorValue{std::array<float, 4>{rgba[0], rgba[1], rgba[2], rgba[3]}},
            k_color_range);
  });
}

void* VkScanoutProducer::vk_instance() const noexcept {
  return static_cast<VkInstance>(impl_->instance);
}
void* VkScanoutProducer::vk_physical_device() const noexcept {
  return static_cast<VkPhysicalDevice>(impl_->physical);
}
void* VkScanoutProducer::vk_device() const noexcept {
  return static_cast<VkDevice>(impl_->device);
}
void* VkScanoutProducer::vk_queue() const noexcept {
  return static_cast<VkQueue>(impl_->queue);
}
void* VkScanoutProducer::vk_image() const noexcept {
  if (impl_->slots.empty()) {
    return nullptr;
  }
  return static_cast<VkImage>(impl_->slots.at(impl_->last_slot)->image);
}
std::uint32_t VkScanoutProducer::vk_format() const noexcept {
  return static_cast<std::uint32_t>(impl_->format);
}
std::uint32_t VkScanoutProducer::queue_family_index() const noexcept {
  return impl_->queue_family;
}

}  // namespace drm::present

#endif  // DRM_CXX_HAS_VK_SCANOUT_PRODUCER
