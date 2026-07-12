// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// gl_compositor.hpp — GPU (GLES2-over-EGL) implementation of
// CompositionTarget. LayerScene's composition fallback uses this instead
// of the CPU CompositeCanvas when an EGL/GLES context can be created on
// the scene's device (Config::Composition::Auto). It uploads each
// unassigned layer's CPU pixels to a texture and SRC_OVER-blends a
// textured quad into a gbm_surface (a GbmSurfaceSource), whose locked
// front buffer is the FB armed onto the canvas plane.
//
// On a split-render SoC (e.g. PowerVR + dc8200 on StarFive) the gbm_device
// is created from the *display* fd, so Mesa's kmsro layer routes rendering
// to the GPU automatically — the validated GBM-on-display pattern.
//
// The class exists only under DRM_CXX_HAS_EGL; the library never links
// libEGL/libGLESv2 (both are dlopen'd via egl_loader / gles_loader), so it
// stays loadable on GPU-less builds where only CompositeCanvas is used.

#pragma once

#include "composite_canvas.hpp"  // CompositeCanvasConfig
#include "composition_target.hpp"

#if DRM_CXX_HAS_EGL

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

class GbmSurfaceSource;
struct AcquiredBuffer;

class GlCompositor : public CompositionTarget {
 public:
  /// Create a GPU compositor for `dev` at `cfg.canvas_width/height` in
  /// `cfg.output_fourcc` (0 => ARGB8888). Returns errc::not_supported when
  /// libEGL/libGLESv2 are unavailable or a context can't be created — the
  /// caller then falls back to CompositeCanvas.
  [[nodiscard]] static drm::expected<std::unique_ptr<GlCompositor>, std::error_code> create(
      const drm::Device& dev, const CompositeCanvasConfig& cfg);

  ~GlCompositor() override;
  GlCompositor(const GlCompositor&) = delete;
  GlCompositor& operator=(const GlCompositor&) = delete;
  GlCompositor(GlCompositor&&) = delete;
  GlCompositor& operator=(GlCompositor&&) = delete;

  void begin_frame() noexcept override;
  void clear() noexcept override;
  void blend(const CompositeSrc& src, const CompositeRect& src_rect,
             const CompositeRect& dst_rect) noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> flush() noexcept override;
  [[nodiscard]] std::uint32_t fb_id() const noexcept override { return fb_id_; }
  [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }
  [[nodiscard]] std::uint32_t drm_fourcc() const noexcept override { return fourcc_; }
  [[nodiscard]] bool armable() const noexcept override { return armable_; }
  [[nodiscard]] bool supports_dma_buf_import(std::uint32_t drm_fourcc) const noexcept override;

  /// True when this compositor can import a layer's dma-buf as an EGLImage and
  /// sample it directly (EGL_EXT_image_dma_buf_import plus eglCreateImageKHR /
  /// glEGLImageTargetTexture2DOES were present at create()). When false, only
  /// the CPU-pixel upload path is available. Probed once in init_egl().
  [[nodiscard]] bool dmabuf_import_supported() const noexcept { return dmabuf_import_supported_; }

  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

  /// Inspection hook: copy the most-recently-flushed front buffer's pixels into
  /// `out` (tightly packed, `drm_fourcc()` layout, width()*height()*4 bytes) by
  /// mapping the gbm bo for read. Returns not_connected if nothing has been
  /// flushed. Used by the gpu_compose_readback probe to verify pixel
  /// correctness (Y-orientation, channel order, blend) without a screen; it
  /// CPU-maps the GPU buffer, so it is not a per-frame fast path.
  [[nodiscard]] drm::expected<void, std::error_code> read_back(std::vector<std::uint8_t>& out);

 private:
  GlCompositor() = default;

  // EGL bring-up over source_->native_device()/surface(); compiles the shader,
  // VBO, texture. Used by create() and on_session_resumed().
  [[nodiscard]] drm::expected<void, std::error_code> init_egl();
  // Tear EGL down (surface/context/display) — must run before source_ dies.
  void teardown_egl() noexcept;

  // Import a single-plane RGB CompositeSrc's dma-buf as an EGLImage
  // (EGL_LINUX_DMA_BUF_EXT) for direct sampling. Returns EGL_NO_IMAGE_KHR when
  // the import can't be done; the caller then falls back to the CPU path. The
  // descriptor's fds are borrowed — not closed here.
  [[nodiscard]] void* import_dma_buf_image(const CompositeSrc& src) noexcept;

  const drm::Device* dev_{nullptr};
  std::unique_ptr<GbmSurfaceSource> source_;
  std::unique_ptr<AcquiredBuffer> held_;  // currently locked front buffer

  void* display_{nullptr};  // EGLDisplay
  void* context_{nullptr};  // EGLContext
  void* surface_{nullptr};  // EGLSurface

  std::uint32_t program_{0};
  std::uint32_t vbo_{0};
  std::uint32_t texture_{0};
  std::int32_t loc_alpha_{-1};
  std::int32_t loc_opaque_{-1};
  std::int32_t loc_tex_{-1};
  std::int32_t loc_bgra_{-1};
  std::int32_t attr_pos_{-1};
  std::int32_t attr_uv_{-1};

  std::uint32_t width_{0};
  std::uint32_t height_{0};
  std::uint32_t fourcc_{0};
  std::uint32_t fb_id_{0};
  bool armable_{false};
  bool dmabuf_import_supported_{false};
  bool frame_open_{false};
  bool allow_software_{false};  // test seam: accept llvmpipe/softpipe/swrast
};

}  // namespace drm::scene

#endif  // DRM_CXX_HAS_EGL
