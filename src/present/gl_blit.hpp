// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// present/gl_blit.hpp — internal
//
// GPU copy of a dma-buf into a scannable surface: the buffer is imported as an
// EGLImage and drawn by GLES into a GlScanoutProducer's gbm_surface, one
// full-screen quad per frame. VkScanoutProducer uses it when the display cannot
// import Vulkan's buffer but the GPU's own GL stack can.
//
// Kept apart from vk_scanout_producer.cpp so that translation unit never sees
// EGL headers: their platform typedefs (and, on some stacks, Xlib macros) would
// differ from or clash with what the Vulkan side includes.

#if DRM_CXX_HAS_EGL

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace drm {
class Device;
}
namespace drm::scene {
class LayerBufferSource;
}

namespace drm::present::detail {

// The dma-buf to draw: one plane.
struct GlBlitInput {
  int fd{-1};  // borrowed; the EGLImage takes its own reference
  std::uint32_t offset{0};
  std::uint32_t pitch{0};
  std::uint64_t modifier{0};
};

class GlBlit {
 public:
  // Builds the scannable surface (`allowed` as for GlScanoutProducer) and the
  // EGLImage over `input`. Fails with not_supported when the EGL/GLES stack
  // lacks dma-buf import.
  [[nodiscard]] static drm::expected<std::unique_ptr<GlBlit>, std::error_code> create(
      drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
      drm::span<const std::uint64_t> allowed, const GlBlitInput& input);
  ~GlBlit();

  GlBlit(const GlBlit&) = delete;
  GlBlit& operator=(const GlBlit&) = delete;
  GlBlit(GlBlit&&) = delete;
  GlBlit& operator=(GlBlit&&) = delete;

  // The scene-facing source (a non-owning proxy over the surface this object
  // owns); callable once. The GlBlit must outlive the scene.
  [[nodiscard]] std::unique_ptr<scene::LayerBufferSource> take_source();

  // Draw the buffer into the surface's back buffer. The producer must have
  // finished writing it.
  [[nodiscard]] bool draw();
  // RGBA of the back buffer's center pixel (after draw()), for self-checks.
  [[nodiscard]] std::optional<std::array<std::uint8_t, 4>> read_center();
  // Post the drawn frame (eglSwapBuffers) for the scene's next acquire.
  [[nodiscard]] drm::expected<void, std::error_code> present();

 private:
  GlBlit();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::present::detail

#endif  // DRM_CXX_HAS_EGL
