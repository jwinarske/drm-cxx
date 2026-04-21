// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

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
