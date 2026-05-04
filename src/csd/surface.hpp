// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/surface.hpp — drm::csd backing buffer for a single decoration.
//
// A Surface owns one CPU-mappable, KMS-scanout-ready ARGB8888 buffer and
// the framebuffer ID that goes with it. The renderer wraps Surface's
// CPU mapping as a Blend2D BLImage and draws straight into scanout; the
// presenter consumes Surface::fb_id() and writes it to the plane's
// FB_ID property at commit time. One Surface backs one decoration; the
// shell creates one per managed document.
//
// Two backing paths converge on the same public API. Surface::create
// tries GBM first (linear ARGB8888 with WRITE+SCANOUT usage) and falls
// back to dumb on GBM unavailability or allocation failure. The chosen
// path is recorded on the Surface and visible via backing(); callers
// that don't care should ignore it. GBM wins on platforms where
// gbm_bo_create produces a buffer the GPU can render into too (future
// EGL/Vulkan content paths plug in cleanly); dumb wins on minimal
// targets and headless test harnesses where no GBM is available.
//
// Public header is intentionally Blend2D-free. The renderer wraps
// paint()'s BufferMapping as a BL_DATA_ACCESS_RW BLImage at the call
// site (using BL_FORMAT_PRGB32, since DRM_FORMAT_ARGB8888 scanout
// expects premultiplied alpha — see capture/snapshot.cpp's bl_format_for
// for the canonical mapping). Consumers that only allocate Surfaces
// without drawing into them don't pull <blend2d.h>.

#pragma once

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/gbm/buffer.hpp>

#include <cstdint>
#include <system_error>
#include <variant>

namespace drm {
class Device;
}  // namespace drm

namespace drm::gbm {
class GbmDevice;
}  // namespace drm::gbm

namespace drm::csd {

/// Allocation parameters for a Surface. V1 is ARGB8888-only; the format
/// field is implicit so future format expansion is a non-breaking
/// addition (an explicit DRM FourCC member can be added without
/// invalidating existing callers).
struct SurfaceConfig {
  std::uint32_t width{0};
  std::uint32_t height{0};
};

/// Which buffer-allocation path produced this Surface. Mostly a
/// diagnostic — callers select behaviour from the public accessors
/// (fb_id, paint, dma_buf_fd), not from the backing tag. Undefined when
/// the Surface is empty().
enum class SurfaceBacking : std::uint8_t {
  Gbm,
  Dumb,
};

class Surface {
 public:
  /// Allocate against `device` and (optionally) `gbm_device`. When
  /// `gbm_device` is non-null the GBM path is tried first; on GBM
  /// allocation / map / AddFB2 failure or when `gbm_device` is null,
  /// the dumb path runs. Returns an error only when both paths
  /// fail or the SurfaceConfig is invalid (zero dimensions).
  ///
  /// `device` must outlive the returned Surface. The buffer holds DRM
  /// handles bound to its fd; if libseat hands the process a fresh fd
  /// during a session-resume cycle, the caller must call forget() on
  /// the Surface and re-allocate against the new Device.
  [[nodiscard]] static drm::expected<Surface, std::error_code> create(
      drm::Device& device, drm::gbm::GbmDevice* gbm_device, const SurfaceConfig& config);

  /// Convenience overload that goes straight to the dumb path. Useful
  /// for headless tests, low-end targets, and any environment where the
  /// caller has a Device but no GbmDevice.
  [[nodiscard]] static drm::expected<Surface, std::error_code> create(drm::Device& device,
                                                                      const SurfaceConfig& config);

  Surface() noexcept = default;
  ~Surface() = default;
  Surface(Surface&&) noexcept = default;
  Surface& operator=(Surface&&) noexcept = default;
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

  /// True if the Surface holds no allocated buffer (default-constructed
  /// or moved-from). All other accessors return zero / undefined values
  /// in that state.
  [[nodiscard]] bool empty() const noexcept;

  /// KMS framebuffer ID for plane assignment. Non-zero when non-empty.
  [[nodiscard]] std::uint32_t fb_id() const noexcept;

  /// Buffer geometry. Width and height are the values from the
  /// SurfaceConfig; stride is whatever the kernel / GBM picked.
  [[nodiscard]] std::uint32_t width() const noexcept;
  [[nodiscard]] std::uint32_t height() const noexcept;
  [[nodiscard]] std::uint32_t stride() const noexcept;

  /// DRM FourCC of the backing pixel data. V1 always returns
  /// DRM_FORMAT_ARGB8888.
  [[nodiscard]] std::uint32_t format() const noexcept;

  /// Which allocation path produced this Surface. Undefined when
  /// empty().
  [[nodiscard]] SurfaceBacking backing() const noexcept;

  /// Acquire a scoped CPU mapping for drawing. Call once per draw
  /// pass; release the returned guard before the surface is scanned
  /// out (Mesa's GBM path does cache-coherence work at unmap, and the
  /// kernel can stage a tiled-to-linear blit there even though we ask
  /// for LINEAR — better to drop the guard explicitly than rely on
  /// it living the right length).
  ///
  /// The renderer wraps the guard's data + stride as a BLImage via
  /// `BLImage::create_from_data(..., BL_FORMAT_PRGB32, data, stride,
  /// BL_DATA_ACCESS_RW, nullptr, nullptr)` — `BL_FORMAT_PRGB32` because
  /// DRM_FORMAT_ARGB8888 scanout expects premultiplied alpha; the
  /// renderer is responsible for producing premultiplied pixels.
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> paint(
      drm::MapAccess access = drm::MapAccess::ReadWrite);

  /// DMA-BUF fd for export to a downstream consumer (out-of-process
  /// renderer, screencast capture, etc.). Caller owns the returned fd
  /// and is responsible for closing it. For the GBM path this calls
  /// `gbm_bo_get_fd`; for the dumb path it issues `drmPrimeHandleToFD`
  /// on the underlying GEM handle. The export is only useful in V1 for
  /// callers that want to plumb the surface to a separate process —
  /// in-process consumers should use the FB_ID directly.
  [[nodiscard]] drm::expected<int, std::error_code> dma_buf_fd() const;

  /// Drop GEM handles and the FB ID without issuing ioctls against the
  /// (now-dead) DRM fd. Mirrors `drm::gbm::Buffer::forget()` /
  /// `drm::dumb::Buffer::forget()`. Use during a libseat session-resume
  /// cycle when the Device's fd has been replaced; the kernel reclaims
  /// the orphaned handles when the old fd closes. After forget() the
  /// Surface is empty().
  void forget() noexcept;

 private:
  Surface(drm::Device* device, drm::gbm::Buffer buffer) noexcept;
  Surface(drm::Device* device, drm::dumb::Buffer buffer) noexcept;

  // The device pointer is non-owning. Used by the dumb path's
  // dma_buf_fd() to issue drmPrimeHandleToFD; the GBM path doesn't
  // need it (gbm_bo_get_fd carries its own fd internally).
  drm::Device* device_{nullptr};
  std::variant<drm::gbm::Buffer, drm::dumb::Buffer> backing_;
};

}  // namespace drm::csd