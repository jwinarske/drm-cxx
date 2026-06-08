// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// present/vk_scanout_producer.hpp
//
// A ScanoutProducer that renders via Vulkan into a dmabuf-exported VkImage and
// feeds it to the scene. libvulkan is loaded at runtime by Vulkan-Hpp's dynamic
// dispatcher (vk::detail::DynamicLoader) -- the library never links -lvulkan.
//
// Buffer strategy: Vulkan allocates the scanout image with an explicit DRM
// format modifier (VK_IMAGE_TILING_DRM_FORMAT_MODIFIER), exports its memory as
// a dmabuf fd, and the fd is wrapped in scene::ExternalDmaBufSource (which
// AddFB2's it). The VkImage's memory backs that dmabuf, so the producer must
// outlive the scene it feeds (declare the producer first / destroy it last).
//
// Single-buffered v1: one VkImage, re-rendered in place each frame (matches the
// vulkan_scene example). render_clear() submits the simplest frame; real
// embedders record their own command buffers against the raw handles below.
//
// Gated on DRM_CXX_HAS_VULKAN (Vulkan headers present at build); the class does
// not exist otherwise. pImpl keeps vulkan.hpp out of this header.

#if DRM_CXX_HAS_VULKAN

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/present/scanout_producer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}

namespace drm::present {

class VkScanoutProducer : public ScanoutProducer {
 public:
  // Borrows `dev`; it must outlive the producer. Builds a VkInstance/VkDevice
  // bound to the same DRM node as `dev`.
  [[nodiscard]] static drm::expected<std::unique_ptr<VkScanoutProducer>, std::error_code> create(
      drm::Device& dev);
  ~VkScanoutProducer() override;

  VkScanoutProducer(const VkScanoutProducer&) = delete;
  VkScanoutProducer& operator=(const VkScanoutProducer&) = delete;
  VkScanoutProducer(VkScanoutProducer&&) = delete;
  VkScanoutProducer& operator=(VkScanoutProducer&&) = delete;

  // The DRM format modifiers Vulkan can render to for `fourcc`
  // (vkGetPhysicalDeviceFormatProperties2 + VkDrmFormatModifierPropertiesListEXT,
  // filtered to color-attachment-capable), most-preferred first. Empty if the
  // format isn't renderable; the backend then falls back to LINEAR.
  [[nodiscard]] std::vector<std::uint64_t> exportable_modifiers(std::uint32_t fourcc) override;

  // Allocate the exportable scanout VkImage with the first usable modifier from
  // `allowed`, export it as a dmabuf, and wrap it in an ExternalDmaBufSource.
  // May be called once; a second call fails with already_connected.
  [[nodiscard]] drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
  create_buffer(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                drm::span<const std::uint64_t> allowed) override;

  // Submit one frame that clears the scanout image to `rgba` (records, submits,
  // waits idle). The scene's next commit scans the result out.
  [[nodiscard]] drm::expected<void, std::error_code> render_clear(std::array<float, 4> rgba);

  // Opaque Vulkan handles (VkDevice / VkQueue / VkImage are pointers) for
  // embedders recording their own command buffers. Null until create_buffer.
  [[nodiscard]] void* vk_device() const noexcept;
  [[nodiscard]] void* vk_queue() const noexcept;
  [[nodiscard]] void* vk_image() const noexcept;
  [[nodiscard]] std::uint32_t queue_family_index() const noexcept;

 private:
  VkScanoutProducer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::present

#endif  // DRM_CXX_HAS_VULKAN
