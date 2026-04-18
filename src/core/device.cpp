// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm.h>
#include <xf86drm.h>

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace drm {

Device::Device(int fd) noexcept : fd_(fd) {}

Device::~Device() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Device::Device(Device&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

Device& Device::operator=(Device&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

drm::expected<Device, std::error_code> Device::open(std::string_view path) {
  std::string const path_str(path);
  int const fd = ::open(path_str.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  // Verify this is actually a DRM device
  auto* version = drmGetVersion(fd);
  if (version == nullptr) {
    ::close(fd);
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }
  drmFreeVersion(version);

  // Set ourselves as DRM master if possible (non-fatal if it fails)
  drmSetMaster(fd);

  return Device(fd);
}

int Device::fd() const noexcept {
  return fd_;
}

drm::expected<void, std::error_code> Device::set_client_cap(uint64_t cap, uint64_t value) const {
  if (fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int const ret = drmSetClientCap(fd_, cap, value);
  if (ret != 0) {
    return drm::unexpected<std::error_code>(
        std::error_code(ret < 0 ? -ret : errno, std::system_category()));
  }
  return {};
}

drm::expected<void, std::error_code> Device::enable_universal_planes() const {
  return set_client_cap(DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
}

drm::expected<void, std::error_code> Device::enable_atomic() const {
  return set_client_cap(DRM_CLIENT_CAP_ATOMIC, 1);
}

}  // namespace drm
