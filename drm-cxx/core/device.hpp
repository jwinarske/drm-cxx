// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>

namespace drm {

class Device {
 public:
  static std::expected<Device, std::error_code> open(std::string_view path);

  [[nodiscard]] int fd() const noexcept;

  std::expected<void, std::error_code> set_client_cap(uint64_t cap, uint64_t value);

  std::expected<void, std::error_code> enable_universal_planes();
  std::expected<void, std::error_code> enable_atomic();

  ~Device();

  Device(Device&&) noexcept;
  Device& operator=(Device&&) noexcept;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

 private:
  explicit Device(int fd) noexcept;
  int fd_{-1};
};

}  // namespace drm
