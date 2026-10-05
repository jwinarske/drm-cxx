// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// gbm_surface_source.hpp — LayerBufferSource backed by a `gbm_surface`
// front-buffer queue that an EGL or Vulkan context renders into.
//
// `GbmBufferSource` is the single-buffer CPU-rendered cousin; this one
// is the multi-buffer GPU-rendered variant. The producer pipeline
// (EGL: `eglCreatePlatformWindowSurface` + `eglSwapBuffers`; Vulkan:
// `VkSurfaceKHR` over the gbm_surface + `vkQueuePresentKHR`) drives
// the swap chain. Each presented frame becomes a "front buffer" that
// the scene picks up here:
//
//   acquire()  → gbm_surface_lock_front_buffer  → drmModeAddFB2WithModifiers
//                                                  (cached per BO)
//   release()  → gbm_surface_release_buffer     (BO returns to free pool)
//
// gbm_surface typically rotates among 2–3 BOs; fb_ids are cached per
// `gbm_bo*` so subsequent presentations of the same BO reuse the
// existing framebuffer registration. The cache is torn down at
// destruction (RmFB on each entry) and on session pause (the DRM fd
// is going away).
//
// Modifier negotiation
// --------------------
//
// `Config::modifier` commits the source to a single DRM format
// modifier the renderer agrees to produce. The surface is created via
// `gbm_surface_create_with_modifiers2` with that single-modifier list
// when present, and via bare `gbm_surface_create` (no modifier hint)
// when `Config::modifier` is `DRM_FORMAT_MOD_INVALID`. `format()`
// reports the same modifier verbatim — the allocator's plane match
// uses it before any BO actually exists.
//
// To negotiate which modifier to pick, call
// `LayerScene::candidate_modifiers(drm_format)` for the union of
// modifiers any plane on the scene's CRTC accepts, intersect with
// the renderer's supported set (EGL: `eglQueryDmaBufModifiersEXT`;
// Vulkan: `VkDrmFormatModifierPropertiesListEXT`), and pick a single
// preferred modifier. Pass that single value as `Config::modifier`.
//
// Threading
// ---------
//
// The producer's render thread and the scene's commit thread both
// touch the underlying gbm_surface — the producer through its
// EGL/Vulkan binding, the scene through `acquire()` / `release()`.
// gbm_surface's lock/release pair is not thread-safe internally, so
// the caller must serialize: typically render + scene-commit run on
// the same thread, with `eglSwapBuffers` (or vkQueuePresentKHR)
// preceding `LayerScene::commit()` each frame.
//
// Format scope
// ------------
//
//   * Single-DRM-plane packed formats only (XRGB8888, ARGB8888,
//     XBGR8888, ABGR8888, RGB565). Semi-planar YUV formats (NV12,
//     P010, etc.) are not the natural GBM-surface target — renderers
//     emit RGB and a downstream YUV-conversion pass is its own concern.
//
// Producer-binding precondition
// -----------------------------
//
// A gbm_surface is an EGL native window: Mesa dispatches
// `lock_front_buffer` / `has_free_buffers` / `release_buffer` into the
// window surface EGL (or Vulkan) creates on it. Until that window exists
// those calls segfault -- on every Mesa device, not a particular driver;
// an initialized EGL display alone is not enough. Destroying the source
// is safe either way. So the scene's commit must NOT call `acquire()`
// before the producer has bound the surface: construct the source, hand
// `native_surface()` to EGL/Vulkan, render one frame, *then* add the
// layer to the scene. Or set `GbmSurfaceConfig::require_bind` and call
// `mark_bound()` once the window surface exists: until then `acquire()`
// returns EAGAIN instead of touching the surface.
//
// Session pause/resume
// --------------------
//
// On `on_session_paused()` the source tears the surface + cached
// fb_ids down. The renderer's EGL/Vulkan binding is no longer valid
// — the producer side must tear its binding down on pause too. On
// `on_session_resumed(new_dev)` the source rebuilds the surface
// against the new DRM fd; the renderer must re-acquire
// `native_surface()` and re-bind. Identity changes verbatim,
// mirroring `EglStreamSource`.

#pragma once

#include "buffer_source.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <memory>
#include <system_error>

struct gbm_device;
struct gbm_surface;

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// Configuration for `GbmSurfaceSource::create`.
struct GbmSurfaceConfig {
  /// Surface dimensions. The renderer's swap chain inherits these.
  std::uint32_t width{0};
  std::uint32_t height{0};

  /// DRM FourCC pixel format. Limited to single-DRM-plane packed
  /// formats — see file comment for scope.
  std::uint32_t drm_format{0};

  /// DRM format modifier the renderer will produce. Use
  /// `DRM_FORMAT_MOD_INVALID` (the sentinel `~0ULL`) to let GBM pick
  /// without a modifier hint — `SourceFormat::modifier` will then
  /// also be `INVALID`. To negotiate a concrete modifier, call
  /// `LayerScene::candidate_modifiers(drm_format)` and pick from
  /// that list.
  std::uint64_t modifier{static_cast<std::uint64_t>(-1)};  // DRM_FORMAT_MOD_INVALID

  /// `GBM_BO_USE_*` flags. Zero selects the source's default
  /// (`GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING`). Callers wanting
  /// a CPU-mappable surface (rare for GPU-rendered scanout) add
  /// `GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE` here.
  std::uint32_t usage{0};

  /// When true, `acquire()` returns EAGAIN until `mark_bound()` is called,
  /// instead of calling into a surface no producer has bound yet (a segfault
  /// in Mesa). Cleared again by `on_session_resumed`. Off by default.
  bool require_bind{false};
};

/// `LayerBufferSource` over a `gbm_surface` front-buffer queue. See
/// file comment for the full contract.
class GbmSurfaceSource : public LayerBufferSource {
 public:
  /// Construct the source. Creates a `gbm_device` over `dev.fd()`
  /// and a `gbm_surface` matching `cfg`. Failure modes:
  ///
  ///   * `errc::invalid_argument` — zero dimensions, zero
  ///     `drm_format`, or an unsupported (non-single-plane) format.
  ///   * `errc::not_supported` — Mesa refused
  ///     `gbm_surface_create_with_modifiers2` for the requested
  ///     (format, modifier) pair, OR the build lacks the symbol
  ///     altogether and `Config::modifier` is set to a non-INVALID
  ///     value.
  ///   * `errc::io_error` — `gbm_create_device` /
  ///     `gbm_surface_create` failure with no specific errno.
  ///   * `errc::bad_file_descriptor` — `dev.fd()` is negative.
  [[nodiscard]] static drm::expected<std::unique_ptr<GbmSurfaceSource>, std::error_code> create(
      const drm::Device& dev, const GbmSurfaceConfig& cfg);

  GbmSurfaceSource(const GbmSurfaceSource&) = delete;
  GbmSurfaceSource& operator=(const GbmSurfaceSource&) = delete;
  GbmSurfaceSource(GbmSurfaceSource&&) = delete;
  GbmSurfaceSource& operator=(GbmSurfaceSource&&) = delete;
  ~GbmSurfaceSource() override;

  /// The raw `gbm_surface*` the renderer binds to. Hand this to
  /// `eglCreatePlatformWindowSurface(EGL_PLATFORM_GBM_KHR, ...)` for
  /// EGL or wrap with `VkSurfaceKHR` over the gbm_surface for
  /// Vulkan. Identity changes across `on_session_resumed` — callers
  /// must re-query after a resume.
  [[nodiscard]] struct gbm_surface* native_surface() const noexcept;

  /// The raw `gbm_device*` the source allocated. Hand this to
  /// `eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_dev, nullptr)`
  /// for EGL or wherever the producer's stack needs the gbm_device
  /// backing the surface. EGL/Vulkan tie surfaces to the gbm_device
  /// they were created on — the producer must use the same instance
  /// returned here, not a sibling gbm_device created against the same
  /// DRM fd. Identity changes across `on_session_resumed` — callers
  /// must re-query after a resume.
  [[nodiscard]] struct gbm_device* native_device() const noexcept;

  /// The producer has created its window surface on `native_surface()`, so
  /// `acquire()` may lock front buffers. Gates `acquire()` only with
  /// `GbmSurfaceConfig::require_bind`; call again after every resume.
  void mark_bound() noexcept;

  // ── LayerBufferSource ────────────────────────────────────────────────

  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override;
  // The most recently acquired BO as a dma-buf (single-plane layouts), so a
  // GPU compositor samples the rendered frame in place. One fd per BO, kept
  // with the BO's FB registration.
  [[nodiscard]] drm::expected<DmaBufDesc, std::error_code> export_dma_buf() override;

  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

  // Stash a render-done sync_file the next acquire() returns as the buffer's
  // acquire fence (the scene wires it to the plane's IN_FENCE_FD, or CPU-waits
  // it). Used by GlScanoutProducer, which exports an EGL native fence per frame.
  void set_acquire_fence(drm::sync::SyncFence fence) noexcept;

 private:
  GbmSurfaceSource();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::scene