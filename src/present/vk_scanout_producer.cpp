// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/vk_scanout_producer.cpp

#include <drm-cxx/present/vk_scanout_producer.hpp>

#if DRM_CXX_HAS_VULKAN

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
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <xf86drm.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
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
  // Queue family the scanout image is released to after each render and
  // acquired back from before the next: the display is an external consumer.
  std::uint32_t foreign_family{VK_QUEUE_FAMILY_EXTERNAL};
  vk::CommandPool cmd_pool;
  vk::CommandBuffer cmd;
  vk::Image image;
  vk::DeviceMemory memory;
  // Display-side scanout buffer, only when the display could not import the
  // Vulkan-exported one (see import_display_buffer); Vulkan then imports this.
  // Declared before the Vulkan objects are torn down in ~Impl, freed after.
  std::optional<gbm::GbmDevice> gbm_dev;
  gbm::Buffer display_gbm;
  dumb::Buffer display_buffer;
  vk::Extent2D extent;

  // How rendered frames reach the display (see create_buffer):
  //   Export — the display imports Vulkan's own buffer (zero-copy);
  //   Import — Vulkan imports a display-side buffer (zero-copy, self-checked);
  //   Copy   — Vulkan renders into host-visible memory and each frame is
  //            memcpy'd into one of two display-side dumb buffers.
  enum class Mode : std::uint8_t { Export, Import, Copy };
  Mode mode{Mode::Export};
  std::array<dumb::Buffer, 2> copy_buffers;
  scene::ExternalDmaBufRing* copy_ring{nullptr};  // non-owning; the scene owns it
  std::array<std::atomic<bool>, 2> copy_slot_free{};
  std::size_t copy_next{0};
  void* copy_mapped{nullptr};
  vk::DeviceSize copy_offset{0};
  vk::DeviceSize copy_row_pitch{0};
  bool copy_coherent{true};
  bool first_frame{true};
  // Non-owning: the scene owns the source; the producer outlives the scene
  // (its VkImage memory backs the dmabuf), so this stays valid.
  scene::ExternalDmaBufSource* vk_source{nullptr};
  vk::Semaphore export_sem;  // signaled by each render submit, exported as sync_file

  // Allocate the scanout buffer on the KMS device and import it into Vulkan
  // as a LINEAR image. Returns the source the scene scans out.
  drm::expected<std::unique_ptr<scene::ExternalDmaBufSource>, std::error_code>
  import_display_buffer(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                        vk::Format format);

  // True when a GPU write through the imported image is visible in the
  // display buffer's memory. Some drivers accept a dma-buf import yet render
  // into private memory; scanning that out would show only the stale buffer.
  bool import_aliases();

  // Host-visible LINEAR render target + two display-side dumb buffers in a ring.
  drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code> setup_copy(
      std::uint32_t width, std::uint32_t height, std::uint32_t fourcc, vk::Format format);

  void release_display_side() {
    display_gbm = gbm::Buffer{};
    display_buffer = dumb::Buffer{};
  }
  vk::Fence reuse_fence;  // CPU-waited before re-recording (image + cmd reuse)

  ~Impl() {
    try {
      if (device) {
        device.waitIdle();
        if (image) {
          device.destroyImage(image);
        }
        if (copy_mapped != nullptr) {
          device.unmapMemory(memory);
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

namespace {

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

drm::expected<std::unique_ptr<scene::ExternalDmaBufSource>, std::error_code>
VkScanoutProducer::Impl::import_display_buffer(std::uint32_t width, std::uint32_t height,
                                               std::uint32_t fourcc, vk::Format format) {
  auto alloc = alloc_display(*dev, gbm_dev, width, height, fourcc);
  if (!alloc) {
    return drm::unexpected<std::error_code>(err(std::errc::not_enough_memory));
  }
  try {
    auto make_image = [&](std::uint32_t pitch) {
      const vk::SubresourceLayout plane_layout{0, 0, pitch, 0, 0};
      vk::StructureChain<vk::ImageCreateInfo, vk::ExternalMemoryImageCreateInfo,
                         vk::ImageDrmFormatModifierExplicitCreateInfoEXT>
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
              vk::ImageDrmFormatModifierExplicitCreateInfoEXT{}
                  .setDrmFormatModifier(DRM_FORMAT_MOD_LINEAR)
                  .setPlaneLayouts(plane_layout)};
      return device.createImage(image_chain.get<vk::ImageCreateInfo>());
    };
    image = make_image(alloc->pitch);
    vk::MemoryRequirements mr = device.getImageMemoryRequirements(image);
    // GPUs commonly pad an image's height (Vivante: to 16 rows), so the image
    // can need more bytes than a width x height buffer holds. Reallocate with
    // enough extra rows at the same pitch; the framebuffer still covers only
    // width x height, the padding rows are never scanned out.
    if (mr.size > alloc->size && alloc->pitch != 0U) {
      const auto rows = static_cast<std::uint32_t>((mr.size + alloc->pitch - 1U) /
                                                   static_cast<vk::DeviceSize>(alloc->pitch));
      auto padded = alloc_display(*dev, gbm_dev, width, rows, fourcc);
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
            vk::MemoryDedicatedAllocateInfo{image, nullptr}};
    try {
      memory = device.allocateMemory(alloc_chain.get<vk::MemoryAllocateInfo>());
    } catch (...) {
      ::close(vk_fd);  // not consumed on failure
      throw;
    }
    device.bindImageMemory(image, memory, 0);
  } catch (const std::exception& e) {
    ::close(alloc->fd);
    if (image) {
      device.destroyImage(image);
      image = nullptr;
    }
    if (memory) {
      device.freeMemory(memory);
      memory = nullptr;
    }
    drm::log_warn("VkScanoutProducer: display-side import failed: {}", e.what());
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  const std::array<scene::ExternalPlaneInfo, 1> planes{
      scene::ExternalPlaneInfo{alloc->fd, 0, alloc->pitch}};
  auto source = scene::ExternalDmaBufSource::create(
      *dev, width, height, fourcc, DRM_FORMAT_MOD_LINEAR,
      drm::span<const scene::ExternalPlaneInfo>(planes.data(), planes.size()));
  ::close(alloc->fd);  // the source dups it
  if (!source) {
    return drm::unexpected<std::error_code>(source.error());
  }
  drm::log_info("VkScanoutProducer: scanout buffer allocated display-side via {}",
                alloc->gbm.empty() ? "a dumb buffer" : "GBM");
  display_gbm = std::move(alloc->gbm);
  display_buffer = std::move(alloc->dumb);
  return source;
}

bool VkScanoutProducer::Impl::import_aliases() {
  constexpr std::uint32_t k_marker = 0x01020304U;
  auto cpu_pixel = [this](std::optional<std::uint32_t> write) -> std::optional<std::uint32_t> {
    if (!display_gbm.empty()) {
      auto m = display_gbm.map(write ? drm::MapAccess::Write : drm::MapAccess::Read);
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
    if (!display_buffer.empty() && display_buffer.data() != nullptr) {
      std::uint32_t v = 0;
      if (write) {
        std::memcpy(display_buffer.data(), &*write, sizeof v);
      }
      std::memcpy(&v, display_buffer.data(), sizeof v);
      return v;
    }
    return std::nullopt;
  };
  if (!cpu_pixel(k_marker).has_value()) {
    return false;
  }
  try {
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
                        {}, {}, {},
                        vk::ImageMemoryBarrier{{},
                                               vk::AccessFlagBits::eTransferWrite,
                                               vk::ImageLayout::eUndefined,
                                               vk::ImageLayout::eGeneral,
                                               VK_QUEUE_FAMILY_IGNORED,
                                               VK_QUEUE_FAMILY_IGNORED,
                                               image,
                                               range});
    cmd.clearColorImage(image, vk::ImageLayout::eGeneral,
                        vk::ClearColorValue{std::array<float, 4>{0.0F, 1.0F, 0.0F, 1.0F}}, range);
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {},
                        vk::ImageMemoryBarrier{vk::AccessFlagBits::eTransferWrite,
                                               {},
                                               vk::ImageLayout::eGeneral,
                                               vk::ImageLayout::eGeneral,
                                               queue_family,
                                               foreign_family,
                                               image,
                                               range});
    cmd.end();
    queue.submit(vk::SubmitInfo{}.setCommandBuffers(cmd), reuse_fence);
    // Left signaled: the next render_clear's 1-frame-behind gate waits on it.
    (void)device.waitForFences(reuse_fence, VK_TRUE, UINT64_MAX);
    cmd.reset();
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer: import self-check failed: {}", e.what());
    return false;
  }
  first_frame = false;  // the image is now GENERAL and released to the display
  const auto after = cpu_pixel(std::nullopt);
  return after.has_value() && *after != k_marker;
}

drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
VkScanoutProducer::Impl::setup_copy(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                                    vk::Format format) {
  try {
    const vk::FormatProperties fp = physical.getFormatProperties(format);
    if (!(fp.linearTilingFeatures & vk::FormatFeatureFlagBits::eTransferDst)) {
      return drm::unexpected<std::error_code>(err(std::errc::not_supported));
    }
    image = device.createImage(vk::ImageCreateInfo{}
                                   .setImageType(vk::ImageType::e2D)
                                   .setFormat(format)
                                   .setExtent({width, height, 1})
                                   .setMipLevels(1)
                                   .setArrayLayers(1)
                                   .setSamples(vk::SampleCountFlagBits::e1)
                                   .setTiling(vk::ImageTiling::eLinear)
                                   .setUsage(vk::ImageUsageFlagBits::eTransferDst)
                                   .setSharingMode(vk::SharingMode::eExclusive)
                                   .setInitialLayout(vk::ImageLayout::eUndefined));
    const vk::MemoryRequirements mr = device.getImageMemoryRequirements(image);
    const vk::PhysicalDeviceMemoryProperties props = physical.getMemoryProperties();
    std::uint32_t type_index = UINT32_MAX;
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      const vk::MemoryPropertyFlags f = props.memoryTypes[i].propertyFlags;
      if ((mr.memoryTypeBits & (1U << i)) == 0U ||
          !(f & vk::MemoryPropertyFlagBits::eHostVisible)) {
        continue;
      }
      const bool coherent = static_cast<bool>(f & vk::MemoryPropertyFlagBits::eHostCoherent);
      if (type_index == UINT32_MAX || coherent) {
        type_index = i;
        copy_coherent = coherent;
      }
      if (coherent) {
        break;
      }
    }
    if (type_index == UINT32_MAX) {
      throw std::runtime_error("no host-visible memory for a LINEAR render target");
    }
    memory = device.allocateMemory(vk::MemoryAllocateInfo{mr.size, type_index});
    device.bindImageMemory(image, memory, 0);
    copy_mapped = device.mapMemory(memory, 0, VK_WHOLE_SIZE);
    const vk::SubresourceLayout layout = device.getImageSubresourceLayout(
        image, vk::ImageSubresource{vk::ImageAspectFlagBits::eColor, 0, 0});
    copy_offset = layout.offset;
    copy_row_pitch = layout.rowPitch;
  } catch (const std::exception& e) {
    drm::log_warn("VkScanoutProducer: copy-path render target: {}", e.what());
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  std::array<int, 2> fds{-1, -1};
  std::array<std::array<scene::ExternalPlaneInfo, 1>, 2> planes{};
  std::array<scene::ExternalSlotDesc, 2> slots{};
  auto close_fds = [&fds] {
    for (int& f : fds) {
      if (f >= 0) {
        ::close(f);
        f = -1;
      }
    }
  };
  for (std::size_t i = 0; i < 2; ++i) {
    auto buf = dumb::Buffer::create(*dev, dumb::Config{width, height, fourcc, 32, false});
    if (!buf ||
        drmPrimeHandleToFD(dev->fd(), buf->handle(), DRM_CLOEXEC | DRM_RDWR, &fds.at(i)) != 0) {
      close_fds();
      return drm::unexpected<std::error_code>(buf ? err(std::errc::io_error) : buf.error());
    }
    planes.at(i).at(0) = scene::ExternalPlaneInfo{fds.at(i), 0, buf->stride()};
    slots.at(i) = scene::ExternalSlotDesc{
        DRM_FORMAT_MOD_LINEAR,
        drm::span<const scene::ExternalPlaneInfo>(planes.at(i).data(), planes.at(i).size())};
    copy_buffers.at(i) = std::move(*buf);
    copy_slot_free.at(i) = true;
  }
  scene::ExternalDmaBufRing::Options opts;
  opts.on_release = [this](std::size_t slot, std::optional<drm::sync::SyncFence> /*fence*/) {
    if (slot < copy_slot_free.size()) {
      copy_slot_free.at(slot) = true;
    }
  };
  auto ring = scene::ExternalDmaBufRing::create(
      *dev, width, height, fourcc,
      drm::span<const scene::ExternalSlotDesc>(slots.data(), slots.size()), std::move(opts));
  close_fds();  // the ring dups them
  if (!ring) {
    return drm::unexpected<std::error_code>(ring.error());
  }
  copy_ring = ring->get();
  mode = Mode::Copy;
  return std::unique_ptr<scene::LayerBufferSource>(std::move(*ring));
}

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
                                      "VK_EXT_image_drm_format_modifier",
                                      "VK_KHR_external_semaphore_fd"};
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
  impl_->extent = vk::Extent2D{width, height};
  if (source) {
    impl_->mode = Impl::Mode::Export;
    // Keep a non-owning handle so render_clear can stash the acquire fence on it.
    impl_->vk_source = (*source).get();
    return std::unique_ptr<scene::LayerBufferSource>(std::move(*source));
  }
  {
    // The display could not import Vulkan's buffer. Typical cause: a display
    // controller without an IOMMU (CMA / GEM-DMA, e.g. i.MX LCDIF) importing a
    // scatter-gather GPU allocation — the PRIME import has to bounce it and
    // fails (ENOMEM). Reverse the direction: let the display allocate (its
    // allocator returns memory it can scan out) and have Vulkan import that,
    // as LINEAR — kept only if a GPU write through the import is actually
    // visible. Otherwise render into host-visible memory and copy each frame.
    drm::log_info("VkScanoutProducer: display could not import the Vulkan buffer ({})",
                  source.error().message());
    impl_->device.destroyImage(impl_->image);
    impl_->device.freeMemory(impl_->memory);
    impl_->image = nullptr;
    impl_->memory = nullptr;

    const auto renderable = exportable_modifiers(fourcc);
    if (std::find(renderable.begin(), renderable.end(), DRM_FORMAT_MOD_LINEAR) !=
        renderable.end()) {
      if (auto imported = impl_->import_display_buffer(width, height, fourcc, format); imported) {
        if (impl_->import_aliases()) {
          drm::log_info(
              "VkScanoutProducer: zero-copy via a display-side buffer imported into "
              "Vulkan");
          impl_->mode = Impl::Mode::Import;
          impl_->vk_source = (*imported).get();
          return std::unique_ptr<scene::LayerBufferSource>(std::move(*imported));
        }
        drm::log_info("VkScanoutProducer: the Vulkan driver does not alias imported dma-bufs");
        imported->reset();
        impl_->device.destroyImage(impl_->image);
        impl_->device.freeMemory(impl_->memory);
        impl_->image = nullptr;
        impl_->memory = nullptr;
        impl_->release_display_side();
      }
    }
    auto copied = impl_->setup_copy(width, height, fourcc, format);
    if (!copied) {
      return drm::unexpected<std::error_code>(copied.error());
    }
    drm::log_info(
        "VkScanoutProducer: no zero-copy path between this GPU and display; each frame is "
        "copied by the CPU into a display-side buffer");
    return copied;
  }
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
    // Acquire from the display (after the first frame the image was released
    // to it), or initialize on the first frame.
    const bool first = impl_->first_frame;
    // The copy path's image never leaves this device; only an exported or
    // imported image changes hands with the display.
    const bool shared = impl_->mode != Impl::Mode::Copy;
    const vk::ImageMemoryBarrier acquire{
        {},
        vk::AccessFlagBits::eTransferWrite,
        first ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        (first || !shared) ? VK_QUEUE_FAMILY_IGNORED : impl_->foreign_family,
        (first || !shared) ? VK_QUEUE_FAMILY_IGNORED : impl_->queue_family,
        impl_->image,
        range};
    impl_->cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, acquire);

    const vk::ClearColorValue clear{std::array<float, 4>{rgba[0], rgba[1], rgba[2], rgba[3]}};
    impl_->cmd.clearColorImage(impl_->image, vk::ImageLayout::eGeneral, clear, range);

    // Release to the display. Without this a GPU may keep the result only in
    // its own compression / fast-clear metadata (Vivante tile status) and the
    // display scans out the untouched memory underneath — a black screen.
    // Copy path: make the result visible to the host read below instead.
    const vk::ImageMemoryBarrier release{
        vk::AccessFlagBits::eTransferWrite,
        shared ? vk::AccessFlags{} : vk::AccessFlags{vk::AccessFlagBits::eHostRead},
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        shared ? impl_->queue_family : VK_QUEUE_FAMILY_IGNORED,
        shared ? impl_->foreign_family : VK_QUEUE_FAMILY_IGNORED,
        impl_->image,
        range};
    impl_->cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        shared ? vk::PipelineStageFlagBits::eBottomOfPipe : vk::PipelineStageFlagBits::eHost, {},
        {}, {}, release);
    impl_->cmd.end();

    if (!shared) {
      impl_->queue.submit(vk::SubmitInfo{}.setCommandBuffers(impl_->cmd), impl_->reuse_fence);
      // Left signaled for the next frame's gate (no reset here).
      (void)impl_->device.waitForFences(impl_->reuse_fence, VK_TRUE, UINT64_MAX);
      if (!impl_->copy_coherent) {
        impl_->device.invalidateMappedMemoryRanges(
            vk::MappedMemoryRange{impl_->memory, 0, VK_WHOLE_SIZE});
      }
      // Write the slot the display is not holding; both held only if frames
      // outrun the flips, in which case the pending one is overwritten.
      std::size_t slot = impl_->copy_next;
      if (!impl_->copy_slot_free.at(slot) && impl_->copy_slot_free.at(slot ^ 1U)) {
        slot ^= 1U;
      }
      dumb::Buffer& dst = impl_->copy_buffers.at(slot);
      const auto* src = static_cast<const std::uint8_t*>(impl_->copy_mapped) + impl_->copy_offset;
      const std::size_t row_bytes = static_cast<std::size_t>(impl_->extent.width) * 4U;
      for (std::uint32_t y = 0; y < impl_->extent.height; ++y) {
        std::memcpy(dst.data() + (static_cast<std::size_t>(y) * dst.stride()),
                    src + (static_cast<std::size_t>(y) * impl_->copy_row_pitch), row_bytes);
      }
      impl_->copy_slot_free.at(slot) = false;
      impl_->copy_ring->submit(slot);
      impl_->copy_next = slot ^ 1U;
      impl_->first_frame = false;
      return {};
    }

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
