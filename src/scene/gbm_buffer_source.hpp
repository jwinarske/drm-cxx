// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// gbm_buffer_source.hpp — single-buffer LayerBufferSource backed by a
// CPU-mappable GBM scanout buffer.
//
// Analogue of DumbBufferSource, but allocates through GBM instead of
// the dumb-buffer ioctl. Suitable for layers where the caller wants
// the GBM allocation path (modifier negotiation, future DMA-BUF
// export, future GPU-rendered variants) while still painting pixels
// from the CPU today. The buffer is created with:
//
//     usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE
//
// so the BO is KMS-scannable, linearly laid out, and CPU-mappable.
// Consumers rasterize through `map(MapAccess::Write | ReadWrite)` and
// drop the guard before committing — gbm_bo_map / gbm_bo_unmap drive
// the driver's cache-coherence and (on tiled platforms)
// staging-buffer lifecycle, so the guard must be held only across the
// CPU access region. The scene submits the cached FB ID from
// `acquire()`.
//
// Single-buffer: the same BO is returned on every `acquire()`. Good
// enough for slideshows and other slowly-rotating content where the
// producer isn't racing the display. For multi-buffered GBM scanout
// (GPU producer, page-flipped display), a ring-based source lands
// once a consumer actually needs it.

#pragma once

#include "buffer_source.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/gbm/buffer.hpp>
#include <drm-cxx/gbm/device.hpp>

#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// `LayerBufferSource` backed by a single CPU-mapped GBM scanout BO,
/// allocated with `GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR |
/// GBM_BO_USE_WRITE`. Same single-buffer semantics as
/// `DumbBufferSource`, but goes through GBM so future variants can
/// negotiate modifiers, export DMA-BUFs, or front a `gbm_surface` for
/// GL/Vulkan producers without changing the source's public shape.
class GbmBufferSource : public LayerBufferSource {
 public:
  /// Allocate a single GBM scanout buffer of the given size and DRM
  /// FourCC format. Usage flags are fixed at `GBM_BO_USE_SCANOUT |
  /// GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE`; modifier is left to the
  /// driver (no explicit pin). The buffer is zero-filled by the
  /// kernel at allocation time, so the first `acquire()` returns a
  /// fully-transparent frame.
  [[nodiscard]] static drm::expected<std::unique_ptr<GbmBufferSource>, std::error_code> create(
      const drm::Device& dev, std::uint32_t width, std::uint32_t height, std::uint32_t drm_format);

  GbmBufferSource(const GbmBufferSource&) = delete;
  GbmBufferSource& operator=(const GbmBufferSource&) = delete;
  GbmBufferSource(GbmBufferSource&&) = delete;
  GbmBufferSource& operator=(GbmBufferSource&&) = delete;
  ~GbmBufferSource() override = default;

  // ── LayerBufferSource ──────────────────────────────────────────────
  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return format_; }
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> map(
      drm::MapAccess access) override;
  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

 private:
  GbmBufferSource(drm::gbm::GbmDevice gbm_dev, drm::gbm::Buffer buffer,
                  SourceFormat format) noexcept
      : gbm_dev_(std::move(gbm_dev)), buffer_(std::move(buffer)), format_(format) {}

  drm::gbm::GbmDevice gbm_dev_;
  drm::gbm::Buffer buffer_;
  SourceFormat format_{};
};

}  // namespace drm::scene
