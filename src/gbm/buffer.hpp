// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// gbm/buffer.hpp — RAII wrapper over a GBM buffer object.
//
// Two allocation paths converge on this type:
//
//   1. `Surface::lock_front_buffer` — the EGL-oriented path. A BO is
//      pulled off the surface's swap chain; destruction returns it via
//      `gbm_surface_release_buffer`.
//
//   2. `Buffer::create(GbmDevice, Config)` — direct allocation via
//      `gbm_bo_create[_with_modifiers2]`. Intended for CPU-rendered
//      scanout: pair with `GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT |
//      GBM_BO_USE_WRITE` so the BO is CPU-mappable with a linear
//      layout, and the scene binds the returned FB ID directly.
//      Destruction calls `gbm_bo_destroy`.
//
// CPU access goes through `map(MapAccess)` — a scoped `gbm_bo_map` /
// `gbm_bo_unmap` pair returned as a `drm::BufferMapping` guard. Mesa
// drivers do cache-coherence work at map and unmap time (and on tiled
// formats, allocate a temporary linear staging buffer for the duration
// of the mapping), so callers must hold the guard only across the
// region of code that actually reads or writes pixels — not across
// the BO's full lifetime.
//
// Factory-allocated buffers optionally carry a KMS FB ID (via
// `drmModeAddFB2WithModifiers`); the FB ID is unwound in the destructor.
// If the owning DRM fd has been replaced out from under the buffer
// (libseat's session-resume cycle), `forget()` drops the handles
// without issuing ioctls against what is now somebody else's fd — the
// same contract `dumb::Buffer::forget()` offers.

#pragma once

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>

struct gbm_bo;
struct gbm_surface;

namespace drm::gbm {

class GbmDevice;

/// Parameters for a direct GBM buffer allocation. `drm_format` is a
/// DRM FourCC (e.g. `DRM_FORMAT_ARGB8888`). `usage` is the bitwise-OR
/// of `GBM_BO_USE_*` flags — at minimum `GBM_BO_USE_SCANOUT` for KMS
/// scanout and `GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE` if the caller
/// intends to CPU-map the BO via `Buffer::map()`. `modifier` is
/// optional: leave unset to let GBM (and the driver) pick a layout,
/// or pin an explicit DRM format modifier (e.g.
/// `DRM_FORMAT_MOD_LINEAR`) to force it. `add_fb` controls whether
/// the factory registers a KMS FB via `drmModeAddFB2WithModifiers`;
/// set false for callers that only want the BO (e.g. to export as
/// DMA-BUF).
struct Config {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t drm_format{0};
  std::uint32_t usage{0};
  std::optional<std::uint64_t> modifier;
  bool add_fb{true};
};

class Buffer {
 public:
  /// Direct BO allocation against `dev`. Returns an error on invalid
  /// config, allocation failure, optional map failure, or optional
  /// AddFB2 failure. On partial failure the factory unwinds what it
  /// built and returns nothing to the caller.
  [[nodiscard]] static drm::expected<Buffer, std::error_code> create(const GbmDevice& dev,
                                                                     const Config& cfg);

  Buffer() noexcept = default;
  ~Buffer();
  Buffer(Buffer&& /*other*/) noexcept;
  Buffer& operator=(Buffer&& /*other*/) noexcept;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  [[nodiscard]] bool empty() const noexcept { return bo_ == nullptr; }

  [[nodiscard]] struct gbm_bo* raw() const noexcept { return bo_; }
  [[nodiscard]] std::uint32_t handle() const noexcept;
  [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] std::uint32_t format() const noexcept { return format_; }
  [[nodiscard]] std::uint64_t modifier() const noexcept { return modifier_; }
  [[nodiscard]] std::size_t size_bytes() const noexcept { return size_bytes_; }

  /// KMS framebuffer ID when the buffer was created with
  /// `Config::add_fb == true`. Zero for surface-locked buffers and
  /// factory-allocated buffers where `add_fb` was false.
  [[nodiscard]] std::uint32_t fb_id() const noexcept { return fb_id_; }

  /// Acquire a scoped CPU mapping. Calls `gbm_bo_map` with the
  /// transfer flags matching `access`; the returned guard's destructor
  /// pairs it with `gbm_bo_unmap`. Mesa drivers use that pair to drive
  /// cache-coherence handling and (for tiled formats) staging-buffer
  /// lifetime, so the guard must be held only across the actual CPU
  /// access — not across the BO's full lifetime. Allocate the BO with
  /// `GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE` (and READ on platforms that
  /// distinguish) for CPU-friendly layouts; tiled BOs map through a
  /// staging buffer the driver populates on map and flushes on unmap.
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> map(drm::MapAccess access);

  /// Get a DMA-BUF fd for this buffer. The caller owns the returned fd
  /// and is responsible for closing it.
  [[nodiscard]] drm::expected<int, std::error_code> fd() const;

  /// Abandon the BO and FB ID without issuing ioctls or GBM calls
  /// against the (now-dead) DRM fd. Same contract as
  /// `dumb::Buffer::forget()`: use when the originating DRM fd has
  /// been replaced by libseat's resume cycle. The kernel reclaims the
  /// orphaned handles on fd close. Any `BufferMapping` previously
  /// acquired against this BO must already have been dropped — the
  /// guard's `gbm_bo_unmap` would otherwise go through the dead fd.
  void forget() noexcept;

 private:
  explicit Buffer(struct gbm_bo* bo, struct gbm_surface* surf = nullptr) noexcept;
  Buffer(int fd, struct gbm_bo* bo, std::uint32_t fb_id, std::size_t size_bytes,
         std::uint32_t width, std::uint32_t height, std::uint32_t stride, std::uint32_t format,
         std::uint64_t modifier) noexcept;

  void destroy() noexcept;

  int fd_{-1};
  struct gbm_bo* bo_{nullptr};
  struct gbm_surface* surf_{nullptr};  // Non-null if locked from a surface
  std::uint32_t fb_id_{0};
  std::size_t size_bytes_{0};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  std::uint32_t stride_{0};
  std::uint32_t format_{0};
  std::uint64_t modifier_{0};

  friend class Surface;
};

}  // namespace drm::gbm
