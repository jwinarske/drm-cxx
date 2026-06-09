// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// present/gl_scanout_producer.cpp

#include <drm-cxx/present/gl_scanout_producer.hpp>
#include <drm-cxx/sync/fence.hpp>

#if DRM_CXX_HAS_EGL

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/egl_loader.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/gbm/device.hpp>
#include <drm-cxx/log.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/gbm_surface_source.hpp>

#include <drm_fourcc.h>
#include <gbm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace drm::present {

namespace {

// Forwards the scene's acquire/release/format calls to a LayerBufferSource the
// producer owns. The scene holds this proxy; the producer owns the real
// GbmSurfaceSource and the EGL state, so the producer (destroyed after the
// scene it feeds) tears EGL down before the gbm_surface the proxy referenced.
class ProxyBufferSource : public scene::LayerBufferSource {
 public:
  explicit ProxyBufferSource(scene::LayerBufferSource* inner) noexcept : inner_(inner) {}

  [[nodiscard]] drm::expected<scene::AcquiredBuffer, std::error_code> acquire() override {
    return inner_->acquire();
  }
  void release(scene::AcquiredBuffer acquired) noexcept override {
    inner_->release(std::move(acquired));
  }
  [[nodiscard]] scene::BindingModel binding_model() const noexcept override {
    return inner_->binding_model();
  }
  [[nodiscard]] scene::SourceFormat format() const noexcept override { return inner_->format(); }
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> map(
      drm::MapAccess access) override {
    return inner_->map(access);
  }

 private:
  scene::LayerBufferSource* inner_;
};

[[nodiscard]] std::error_code err(std::errc code) noexcept {
  return std::make_error_code(code);
}

}  // namespace

drm::expected<std::unique_ptr<GlScanoutProducer>, std::error_code> GlScanoutProducer::create(
    drm::Device& dev) {
  const auto& egl = drm::detail::egl_loader();
  if (!egl.loaded || (egl.get_platform_display_core == nullptr)) {
    drm::log_warn("GlScanoutProducer: libEGL unavailable — cannot create a GL producer");
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  return std::unique_ptr<GlScanoutProducer>(new GlScanoutProducer(dev));
}

GlScanoutProducer::GlScanoutProducer(drm::Device& dev) noexcept : dev_(&dev) {}

GlScanoutProducer::~GlScanoutProducer() {
  // EGL teardown first, while the gbm_surface (owned by source_, destroyed
  // after this body) is still alive.
  const auto& egl = drm::detail::egl_loader();
  auto* const display = static_cast<EGLDisplay>(display_);
  if (display != nullptr) {
    if (egl.make_current != nullptr) {
      egl.make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if ((surface_ != nullptr) && (egl.destroy_surface != nullptr)) {
      egl.destroy_surface(display, static_cast<EGLSurface>(surface_));
    }
    if ((context_ != nullptr) && (egl.destroy_context != nullptr)) {
      egl.destroy_context(display, static_cast<EGLContext>(context_));
    }
    if (egl.terminate != nullptr) {
      egl.terminate(display);
    }
  }
}

std::vector<std::uint64_t> GlScanoutProducer::exportable_modifiers(std::uint32_t fourcc) {
  const auto& egl = drm::detail::egl_loader();
  if (!egl.loaded || (egl.get_platform_display_core == nullptr) || (egl.initialize == nullptr) ||
      (egl.terminate == nullptr)) {
    return {};
  }
  auto gbm = drm::gbm::GbmDevice::create(dev_->fd());
  if (!gbm) {
    return {};
  }
  EGLDisplay display = egl.get_platform_display_core(EGL_PLATFORM_GBM_KHR, (*gbm).raw(), nullptr);
  if ((display == EGL_NO_DISPLAY) || (egl.initialize(display, nullptr, nullptr) != EGL_TRUE)) {
    return {};
  }

  std::vector<std::uint64_t> out;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto query = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
      egl.get_proc_address("eglQueryDmaBufModifiersEXT"));
  if (query != nullptr) {
    const auto format = static_cast<EGLint>(fourcc);
    EGLint count = 0;
    if ((query(display, format, 0, nullptr, nullptr, &count) == EGL_TRUE) && (count > 0)) {
      std::vector<EGLuint64KHR> mods(static_cast<std::size_t>(count));
      std::vector<EGLBoolean> external(static_cast<std::size_t>(count));
      if (query(display, format, count, mods.data(), external.data(), &count) == EGL_TRUE) {
        out.assign(mods.begin(), mods.begin() + count);
      }
    }
  }
  egl.terminate(display);
  return out;
}

drm::expected<std::unique_ptr<scene::LayerBufferSource>, std::error_code>
GlScanoutProducer::create_buffer(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                                 drm::span<const std::uint64_t> allowed) {
  if (source_) {
    return drm::unexpected<std::error_code>(err(std::errc::already_connected));
  }
  const auto& egl = drm::detail::egl_loader();
  if (!egl.loaded) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }

  // The backend hands us the negotiated set (producer ∩ plane); take the most
  // preferred. Empty -> INVALID, letting GBM pick its own layout.
  scene::GbmSurfaceConfig cfg;
  cfg.width = width;
  cfg.height = height;
  cfg.drm_format = fourcc;
  cfg.modifier = allowed.empty() ? DRM_FORMAT_MOD_INVALID : allowed[0];
  cfg.usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
  auto src = scene::GbmSurfaceSource::create(*dev_, cfg);
  if (!src) {
    return drm::unexpected<std::error_code>(src.error());
  }
  source_ = std::move(*src);

  EGLDisplay display =
      egl.get_platform_display_core(EGL_PLATFORM_GBM_KHR, source_->native_device(), nullptr);
  if ((display == EGL_NO_DISPLAY) || (egl.initialize(display, nullptr, nullptr) != EGL_TRUE)) {
    source_.reset();
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  display_ = display;
  if (egl.bind_api(EGL_OPENGL_ES_API) != EGL_TRUE) {
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  // Pick the EGLConfig whose native visual matches the gbm_surface format; Mesa
  // sorts "best first" without filtering on EGL_NATIVE_VISUAL_ID, so walk.
  const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE,
                              EGL_WINDOW_BIT,
                              EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES2_BIT,
                              EGL_RED_SIZE,
                              8,
                              EGL_GREEN_SIZE,
                              8,
                              EGL_BLUE_SIZE,
                              8,
                              EGL_ALPHA_SIZE,
                              8,
                              EGL_NONE};
  EGLint num_configs = 0;
  if ((egl.choose_config(display, cfg_attrs, nullptr, 0, &num_configs) != EGL_TRUE) ||
      (num_configs == 0)) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }
  std::vector<EGLConfig> configs(static_cast<std::size_t>(num_configs));
  egl.choose_config(display, cfg_attrs, configs.data(), num_configs, &num_configs);
  EGLConfig chosen = nullptr;
  for (auto* const candidate : configs) {
    EGLint visual_id = 0;
    if ((egl.get_config_attrib(display, candidate, EGL_NATIVE_VISUAL_ID, &visual_id) == EGL_TRUE) &&
        (static_cast<std::uint32_t>(visual_id) == fourcc)) {
      chosen = candidate;
      break;
    }
  }
  if (chosen == nullptr) {
    return drm::unexpected<std::error_code>(err(std::errc::not_supported));
  }

  const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = egl.create_context(display, chosen, EGL_NO_CONTEXT, ctx_attrs);
  if (context == EGL_NO_CONTEXT) {
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  context_ = context;

  EGLSurface surface = EGL_NO_SURFACE;
  if (egl.create_platform_window_surface != nullptr) {
    surface =
        egl.create_platform_window_surface(display, chosen, source_->native_surface(), nullptr);
  }
  if (surface == EGL_NO_SURFACE) {
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  surface_ = surface;

  return std::unique_ptr<scene::LayerBufferSource>(new ProxyBufferSource(source_.get()));
}

drm::expected<void, std::error_code> GlScanoutProducer::make_current() {
  const auto& egl = drm::detail::egl_loader();
  if ((display_ == nullptr) || (context_ == nullptr) || (surface_ == nullptr)) {
    return drm::unexpected<std::error_code>(err(std::errc::not_connected));
  }
  if (egl.make_current(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_),
                       static_cast<EGLSurface>(surface_),
                       static_cast<EGLContext>(context_)) != EGL_TRUE) {
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }
  return {};
}

drm::expected<void, std::error_code> GlScanoutProducer::swap_buffers() {
  const auto& egl = drm::detail::egl_loader();
  if ((display_ == nullptr) || (surface_ == nullptr)) {
    return drm::unexpected<std::error_code>(err(std::errc::not_connected));
  }
  auto* dpy = static_cast<EGLDisplay>(display_);

  // Insert a native-fence sync capturing the frame's draw BEFORE the swap;
  // eglSwapBuffers flushes the queued GL commands, which is what makes the
  // exported fd valid. The fd then signals when the GPU finishes the frame — we
  // hand it to the source as this buffer's acquire fence so KMS waits on the
  // render (or the scene CPU-waits it) instead of relying on implicit sync.
  // Null entry points (stacks without EGL_ANDROID_native_fence_sync) just skip
  // the export and fall back to the driver's implicit ordering.
  EGLSyncKHR sync = EGL_NO_SYNC_KHR;
  if ((egl.create_sync != nullptr) && (egl.dup_native_fence_fd != nullptr)) {
    sync = egl.create_sync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
  }

  if (egl.swap_buffers(dpy, static_cast<EGLSurface>(surface_)) != EGL_TRUE) {
    if ((sync != EGL_NO_SYNC_KHR) && (egl.destroy_sync != nullptr)) {
      egl.destroy_sync(dpy, sync);
    }
    return drm::unexpected<std::error_code>(err(std::errc::io_error));
  }

  if (sync != EGL_NO_SYNC_KHR) {
    const int fence_fd = egl.dup_native_fence_fd(dpy, sync);
    egl.destroy_sync(dpy, sync);
    if (fence_fd >= 0) {
      if (auto fence = drm::sync::SyncFence::import_fd(fence_fd); fence && (source_ != nullptr)) {
        source_->set_acquire_fence(std::move(*fence));
      }
      ::close(fence_fd);  // import_fd dups; close ours
    }
  }
  return {};
}

}  // namespace drm::present

#endif  // DRM_CXX_HAS_EGL
