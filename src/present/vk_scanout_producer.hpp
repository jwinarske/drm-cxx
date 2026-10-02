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
// Frames are drawn through render(): the producer opens a command buffer, hands
// it and the frame's VkImage to the caller's recorder, then does whatever this
// GPU/display pair needs to get the result on screen. When the display scans
// Vulkan's memory (export / import), it rotates among Options::buffer_count
// images so the next frame never draws into the one on screen. Otherwise each
// frame is copied — by GL on the GPU, or by the CPU — into a display-side
// buffer (see create_buffer). render_clear() is the simplest such frame.
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
#include <functional>
#include <memory>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}

namespace drm::present {

class VkScanoutProducer : public ScanoutProducer {
 public:
  struct Options {
    // Images rotated among when the display scans Vulkan's memory directly.
    // Three lets the scene keep one on screen and one queued while the next
    // renders. Ignored by the GPU- and CPU-copy paths, which render into one.
    std::uint32_t buffer_count{3};
  };

  // Records one frame. `command_buffer` (VkCommandBuffer) is recording, outside
  // any render pass; `image` (VkImage, the format of vk_format(), the size given
  // to create_buffer) is in VK_IMAGE_LAYOUT_GENERAL and must be left there —
  // e.g. a render pass with initialLayout/finalLayout GENERAL. Draw the whole
  // frame: the image may be a different one from the previous call, and its
  // contents are not preserved. The image allows color-attachment and transfer
  // use. Barriers into and out of the frame are the producer's.
  using Recorder = std::function<void(void* command_buffer, void* image)>;

  // Borrows `dev`; it must outlive the producer. Builds a VkInstance/VkDevice
  // bound to the same DRM node as `dev`.
  [[nodiscard]] static drm::expected<std::unique_ptr<VkScanoutProducer>, std::error_code> create(
      drm::Device& dev);
  [[nodiscard]] static drm::expected<std::unique_ptr<VkScanoutProducer>, std::error_code> create(
      drm::Device& dev, const Options& options);
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

  // Record (see Recorder) and submit one frame; the scene's next commit scans
  // it out. Needs create_buffer first. On the copy paths this waits for the
  // GPU; on the zero-copy ones the display waits on a fence instead.
  [[nodiscard]] drm::expected<void, std::error_code> render(const Recorder& record);

  // One frame that clears the scanout image to `rgba`.
  [[nodiscard]] drm::expected<void, std::error_code> render_clear(std::array<float, 4> rgba);

  // Opaque Vulkan handles (VkInstance / VkPhysicalDevice / VkDevice / VkQueue
  // are pointers) for building pipelines, uploading textures and so on against
  // the producer's device. Valid from create().
  [[nodiscard]] void* vk_instance() const noexcept;
  [[nodiscard]] void* vk_physical_device() const noexcept;
  [[nodiscard]] void* vk_device() const noexcept;
  [[nodiscard]] void* vk_queue() const noexcept;
  [[nodiscard]] std::uint32_t queue_family_index() const noexcept;
  // VkFormat of the scanout images (valid after create_buffer).
  [[nodiscard]] std::uint32_t vk_format() const noexcept;
  // The image most recently rendered; null before create_buffer. Draw through
  // render() rather than into this: the frame being scanned out changes.
  [[nodiscard]] void* vk_image() const noexcept;

 private:
  VkScanoutProducer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::present

#endif  // DRM_CXX_HAS_VULKAN
