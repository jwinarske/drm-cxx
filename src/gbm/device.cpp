// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <gbm.h>

#include <cerrno>
#include <system_error>

namespace drm::gbm {

GbmDevice::GbmDevice(struct gbm_device* dev) noexcept : dev_(dev) {}

GbmDevice::~GbmDevice() {
  if (dev_ != nullptr) {
    gbm_device_destroy(dev_);
  }
}

GbmDevice::GbmDevice(GbmDevice&& other) noexcept : dev_(other.dev_) {
  other.dev_ = nullptr;
}

GbmDevice& GbmDevice::operator=(GbmDevice&& other) noexcept {
  if (this != &other) {
    if (dev_ != nullptr) {
      gbm_device_destroy(dev_);
    }
    dev_ = other.dev_;
    other.dev_ = nullptr;
  }
  return *this;
}

drm::expected<GbmDevice, std::error_code> GbmDevice::create(int drm_fd) {
  if (drm_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  auto* dev = gbm_create_device(drm_fd);
  if (dev == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  return GbmDevice(dev);
}

struct gbm_device* GbmDevice::raw() const noexcept {
  return dev_;
}

}  // namespace drm::gbm
