// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// present/gl_scanout_producer.hpp
//
// A ScanoutProducer that renders via EGL/GLES into a gbm_surface and feeds the
// locked front buffer to the scene. libEGL is dlopen'd at runtime
// (drm::detail::egl_loader) -- the library never links -lEGL. GLES rendering is
// the caller's: the producer sets up the EGL context + window surface and
// exposes make_current()/swap_buffers(); the embedder issues the GL draw calls.
//
// Lifetime: the producer OWNS the gbm_surface (a scene::GbmSurfaceSource) and
// the EGL display/context/window-surface, and hands the scene a non-owning
// proxy LayerBufferSource. So the natural teardown order -- scene (proxy)
// first, producer last -- destroys the EGL surface before the gbm_surface it
// wraps. The producer must therefore outlive the scene it feeds.
//
// Gated on DRM_CXX_HAS_EGL (EGL headers present at build); the class simply
// does not exist otherwise. The header keeps EGL handles as void* so it carries
// no EGL header dependency of its own.

#if DRM_CXX_HAS_EGL

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/present/scanout_producer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}
namespace drm::scene {
class GbmSurfaceSource;
}

namespace drm::present {

class GlScanoutProducer : public ScanoutProducer {
 public:
  // Borrows `dev`; it must outlive the producer.
  [[nodiscard]] static drm::expected<std::unique_ptr<GlScanoutProducer>, std::error_code> create(
      drm::Device& dev);
  ~GlScanoutProducer() override;

  GlScanoutProducer(const GlScanoutProducer&) = delete;
  GlScanoutProducer& operator=(const GlScanoutProducer&) = delete;
  GlScanoutProducer(GlScanoutProducer&&) = delete;
  GlScanoutProducer& operator=(GlScanoutProducer&&) = delete;

  // The GL-renderable modifiers EGL advertises for `fourcc`
  // (eglQueryDmaBufModifiersEXT), most-preferred first. Empty when EGL
  // cannot be probed; the backend then falls back to LINEAR.
  [[nodiscard]] std::vector<std::uint64_t> exportable_modifiers(std::uint32_t fourcc) override;

  // Build the gbm_surface (with the first usable modifier from `allowed`) and
  // its EGL context + window surface. Returns a non-owning proxy over the
  // producer-owned source. May be called once; a second call fails with
  // already_connected.
  [[nodiscard]] drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
  create_buffer(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                drm::span<const std::uint64_t> allowed) override;

  // Make the producer's EGL context current on its window surface; the caller
  // then issues GLES draw calls and finishes with swap_buffers().
  [[nodiscard]] drm::expected<void, std::error_code> make_current();
  // Post the rendered frame to the gbm_surface (eglSwapBuffers); the scene's
  // next acquire() locks it as the scanout front buffer.
  [[nodiscard]] drm::expected<void, std::error_code> swap_buffers();

  // Opaque EGL handles (EGLDisplay / EGLContext are void* typedefs) for callers
  // that drive GLES directly. EGL_NO_DISPLAY / EGL_NO_CONTEXT until create_buffer.
  [[nodiscard]] void* egl_display() const noexcept { return display_; }
  [[nodiscard]] void* egl_context() const noexcept { return context_; }

 private:
  explicit GlScanoutProducer(drm::Device& dev) noexcept;

  drm::Device* dev_;
  std::unique_ptr<scene::GbmSurfaceSource> source_;
  void* display_{nullptr};  // EGLDisplay
  void* context_{nullptr};  // EGLContext
  void* surface_{nullptr};  // EGLSurface
};

}  // namespace drm::present

#endif  // DRM_CXX_HAS_EGL
