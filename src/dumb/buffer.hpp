// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// dumb/buffer.hpp — RAII wrapper over the DRM "dumb buffer" ioctls.
//
// Dumb buffers are KMS's GPU-free allocation path: any driver implementing
// DRM_CAP_DUMB_BUFFER (i.e. every modern KMS driver) can allocate a CPU-
// mappable linear pixel buffer with no accelerator involvement. They are
// the right tool for cursors, CSD, software-rendered UI, and test
// harnesses — anything where a simple ARGB8888 scanout surface beats the
// complexity of a GBM + GPU path.
//
// `drm::dumb::Buffer` centralizes the four-step allocation ceremony
// (DRM_IOCTL_MODE_CREATE_DUMB → DRM_IOCTL_MODE_MAP_DUMB → mmap →
// drmModeAddFB2) behind a move-only value. The destructor unwinds it
// (drmModeRmFB → munmap → DRM_IOCTL_MODE_DESTROY_DUMB). If the owning
// DRM fd has been replaced out from under the buffer (e.g. libseat's
// session-resume cycle), `forget()` drops the handles without issuing
// ioctls against what is now somebody else's fd.

#pragma once

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <system_error>

namespace drm {
class Device;
}  // namespace drm

namespace drm::dumb {

/// Parameters for a dumb-buffer allocation. `drm_format` is a DRM FourCC
/// (e.g. `DRM_FORMAT_ARGB8888`) — `<drm_fourcc.h>` is not pulled into
/// this header; the .cpp validates it. `bpp` is the bits-per-pixel the
/// kernel uses to compute the allocation size; it must match the format.
/// `add_fb` controls whether the factory calls `drmModeAddFB2` after
/// allocation — set to false for the legacy `drmModeSetCursor` path,
/// which consumes the raw GEM handle and does not need an FB ID.
struct Config {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t drm_format{0};
  std::uint32_t bpp{32};
  bool add_fb{true};
};

/// Owning handle to a dumb buffer + (optionally) its KMS framebuffer +
/// its CPU mapping. Move-only value type. A default-constructed Buffer
/// holds no resources (`empty() == true`).
class Buffer {
 public:
  /// Allocate a dumb buffer on `dev`'s DRM fd per `cfg`. Returns an
  /// error on ioctl failure, mmap failure, or invalid config
  /// (zero-sized, unknown format, mismatched bpp).
  [[nodiscard]] static drm::expected<Buffer, std::error_code> create(const drm::Device& dev,
                                                                     const Config& cfg);

  Buffer() noexcept = default;
  ~Buffer();

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;

  [[nodiscard]] bool empty() const noexcept { return gem_handle_ == 0; }

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }
  [[nodiscard]] std::size_t size_bytes() const noexcept { return size_bytes_; }

  /// GEM handle for this buffer on its originating DRM fd. Non-zero
  /// when non-empty. Legacy cursor paths (`drmModeSetCursor`) consume
  /// this directly; atomic paths use `fb_id()` instead.
  [[nodiscard]] std::uint32_t handle() const noexcept { return gem_handle_; }

  /// KMS framebuffer ID if the buffer was created with `Config::add_fb`
  /// true. Zero if `add_fb` was false or the buffer is empty.
  [[nodiscard]] std::uint32_t fb_id() const noexcept { return fb_id_; }

  /// Mutable CPU mapping over the buffer's linear pixel storage.
  /// `size_bytes()` long. Always null when `empty()`.
  [[nodiscard]] std::uint8_t* data() noexcept { return mapped_; }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return mapped_; }

  /// Acquire a scoped CPU mapping. For dumb buffers the underlying
  /// mmap is held for the buffer's full lifetime and is cache-coherent
  /// by kernel construction, so this is effectively a thin wrapper
  /// around `data()` / `stride()` — the returned guard's destructor is
  /// a no-op. The unified shape exists so consumers can write the same
  /// scoped paint code against dumb and GBM buffers; the access mode
  /// is recorded on the guard but ignored at the dumb-buffer level.
  /// Empty (returns an empty mapping) when the buffer is empty.
  [[nodiscard]] drm::BufferMapping map(drm::MapAccess access) noexcept;

  /// Abandon the GEM handle and FB ID without issuing ioctls. Use when
  /// the originating DRM fd is known to be dead (libseat session-resume
  /// replaced the Device). The CPU mapping is still torn down here —
  /// munmap is fd-independent — so the caller is left with a valid
  /// empty() buffer. The kernel reclaims the orphaned handles on fd
  /// close.
  void forget() noexcept;

 private:
  Buffer(int fd, std::uint32_t gem_handle, std::uint32_t fb_id, std::uint8_t* mapped,
         std::size_t size_bytes, std::uint32_t width, std::uint32_t height,
         std::uint32_t stride) noexcept;

  void destroy() noexcept;

  int fd_{-1};
  std::uint32_t gem_handle_{0};
  std::uint32_t fb_id_{0};
  std::uint8_t* mapped_{nullptr};
  std::size_t size_bytes_{0};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  std::uint32_t stride_{0};
};

}  // namespace drm::dumb
