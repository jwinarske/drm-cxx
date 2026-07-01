// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace drm {

Device::Device(int fd, bool owns_fd) noexcept : fd_(fd), owns_fd_(owns_fd) {}

Device::~Device() {
  if (fd_ >= 0 && owns_fd_) {
    ::close(fd_);
  }
}

Device::Device(Device&& other) noexcept : fd_(other.fd_), owns_fd_(other.owns_fd_) {
  other.fd_ = -1;
  other.owns_fd_ = false;
}

Device& Device::operator=(Device&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0 && owns_fd_) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    owns_fd_ = other.owns_fd_;
    other.fd_ = -1;
    other.owns_fd_ = false;
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

  return Device(fd, /*owns_fd=*/true);
}

Device Device::from_fd(const int fd) noexcept {
  return {fd, /*owns_fd=*/false};
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

namespace {
std::error_code errno_ec(int ret) {
  return {ret < 0 && ret != -1 ? -ret : errno, std::system_category()};
}
}  // namespace

drm::expected<void, std::error_code> Device::set_master() const {
  if (fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  if (drmSetMaster(fd_) != 0) {
    return drm::unexpected<std::error_code>(errno_ec(-1));
  }
  return {};
}

drm::expected<void, std::error_code> Device::drop_master() const {
  if (fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  if (drmDropMaster(fd_) != 0) {
    return drm::unexpected<std::error_code>(errno_ec(-1));
  }
  return {};
}

drm::expected<std::uint32_t, std::error_code> Device::add_framebuffer(
    std::uint32_t width, std::uint32_t height, std::uint32_t fourcc, const std::uint32_t handles[4],
    const std::uint32_t strides[4], const std::uint32_t offsets[4],
    const std::uint64_t modifiers[4], std::uint32_t flags) const {
  if (fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  std::uint32_t fb_id = 0;
  const int ret = drmModeAddFB2WithModifiers(fd_, width, height, fourcc, handles, strides, offsets,
                                             modifiers, &fb_id, flags);
  if (ret != 0 || fb_id == 0) {
    return drm::unexpected<std::error_code>(errno_ec(ret));
  }
  return fb_id;
}

drm::expected<void, std::error_code> Device::remove_framebuffer(std::uint32_t fb_id) const {
  if (fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  if (drmModeRmFB(fd_, fb_id) != 0) {
    return drm::unexpected<std::error_code>(errno_ec(-1));
  }
  return {};
}

drm::expected<void, std::error_code> Device::commit_atomic(drmModeAtomicReq* request,
                                                           std::uint32_t flags,
                                                           void* user_data) const {
  if (fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  if (request == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  const int ret = drmModeAtomicCommit(fd_, request, flags, user_data);
  if (ret != 0) {
    return drm::unexpected<std::error_code>(errno_ec(ret));
  }
  return {};
}

}  // namespace drm
