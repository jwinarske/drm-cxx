// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "device.hpp"

namespace drm {

Device::Device(int fd) noexcept : fd_(fd) {}

Device::~Device() = default;

Device::Device(Device&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

Device& Device::operator=(Device&& other) noexcept {
  if (this != &other) {
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

std::expected<Device, std::error_code>
Device::open([[maybe_unused]] std::string_view path) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

int Device::fd() const noexcept { return fd_; }

std::expected<void, std::error_code>
Device::set_client_cap([[maybe_unused]] uint64_t cap,
                       [[maybe_unused]] uint64_t value) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

std::expected<void, std::error_code> Device::enable_universal_planes() {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

std::expected<void, std::error_code> Device::enable_atomic() {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace drm
