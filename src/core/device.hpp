// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <xf86drmMode.h>

#include <cstdint>
#include <string_view>
#include <system_error>

namespace drm {

class Device {
 public:
  static drm::expected<Device, std::error_code> open(std::string_view path);

  /// Wrap an already-open DRM fd owned by someone else. The returned
  /// Device does NOT close the fd on destruction; the caller (e.g. a
  /// seat session holding a revocable libseat-managed fd) retains the
  /// lifetime responsibility. Resume flows replace the Device by
  /// move-assigning a freshly constructed from_fd(new_fd).
  [[nodiscard]] static Device from_fd(int fd) noexcept;

  [[nodiscard]] int fd() const noexcept;

  [[nodiscard]] drm::expected<void, std::error_code> set_client_cap(uint64_t cap,
                                                                    uint64_t value) const;

  [[nodiscard]] drm::expected<void, std::error_code> enable_universal_planes() const;
  [[nodiscard]] drm::expected<void, std::error_code> enable_atomic() const;

  // ── Low-level borrowed-resource KMS ops ──────────────────────────────────
  // Let an external owner of the request/buffers (e.g. a Chromium ozone/drm
  // backend) drive KMS through this Device. They operate on resources the caller
  // already built; drm-cxx's AtomicRequest / ScanoutBuffer own those for the
  // library's own higher-level flows. A KMS backend built on drm-cxx uses these
  // to drive a request and buffers it owns itself (see docs/gn-integration.md).

  [[nodiscard]] drm::expected<void, std::error_code> set_master() const;
  [[nodiscard]] drm::expected<void, std::error_code> drop_master() const;

  /// Create a KMS framebuffer from already-imported GEM handles (mirrors
  /// drmModeAddFB2WithModifiers). `flags` carries DRM_MODE_FB_MODIFIERS when the
  /// modifiers[] are meaningful. Returns the new fb id.
  [[nodiscard]] drm::expected<std::uint32_t, std::error_code> add_framebuffer(
      std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
      const std::uint32_t handles[4], const std::uint32_t strides[4],
      const std::uint32_t offsets[4], const std::uint64_t modifiers[4], std::uint32_t flags) const;

  [[nodiscard]] drm::expected<void, std::error_code> remove_framebuffer(std::uint32_t fb_id) const;

  /// Commit a caller-built atomic request through this device (mirrors
  /// drmModeAtomicCommit). `user_data` round-trips to the page-flip handler.
  [[nodiscard]] drm::expected<void, std::error_code> commit_atomic(drmModeAtomicReq* request,
                                                                   std::uint32_t flags,
                                                                   void* user_data) const;

  ~Device();

  Device(Device&& /*other*/) noexcept;
  Device& operator=(Device&& /*other*/) noexcept;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

 private:
  Device(int fd, bool owns_fd) noexcept;
  int fd_{-1};
  bool owns_fd_{true};
};

}  // namespace drm
