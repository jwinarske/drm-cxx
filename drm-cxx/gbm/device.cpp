// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "device.hpp"

namespace drm::gbm {

GbmDevice::GbmDevice(struct gbm_device* dev) noexcept : dev_(dev) {}
GbmDevice::~GbmDevice() = default;

GbmDevice::GbmDevice(GbmDevice&& other) noexcept : dev_(other.dev_) {
  other.dev_ = nullptr;
}

GbmDevice& GbmDevice::operator=(GbmDevice&& other) noexcept {
  if (this != &other) {
    dev_ = other.dev_;
    other.dev_ = nullptr;
  }
  return *this;
}

std::expected<GbmDevice, std::error_code> GbmDevice::create([[maybe_unused]] int drm_fd) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

struct gbm_device* GbmDevice::raw() const noexcept { return dev_; }

} // namespace drm::gbm
