// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <gbm.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <system_error>
#include <unordered_set>

namespace drm::gbm {

namespace {

struct Retained {
  std::mutex mu;
  std::unordered_set<const struct gbm_device*> devices;
};

// Never destroyed: GbmDevice destructors may run during static destruction.
Retained& retained() {
  static auto* r = new Retained();  // NOLINT(cppcoreguidelines-owning-memory)
  return *r;
}

}  // namespace

bool retain_for_egl(struct gbm_device* dev, const char* egl_vendor) noexcept {
  if (dev == nullptr) {
    return false;
  }
  // Vivante's libEGL pairs with its own gbm backend ("viv"), which identifies
  // the stack even when eglInitialize failed and there is no vendor string.
  const char* const backend = gbm_device_get_backend_name(dev);
  const bool vivante = (egl_vendor != nullptr && std::strstr(egl_vendor, "Vivante") != nullptr) ||
                       (backend != nullptr && std::strcmp(backend, "viv") == 0);
  if (!vivante) {
    return false;
  }
  try {
    Retained& r = retained();
    const std::lock_guard<std::mutex> lock(r.mu);
    r.devices.insert(dev);
  } catch (...) {
    return false;  // allocation failure: fall back to the normal lifetime
  }
  return true;
}

bool retained_for_egl(const struct gbm_device* dev) noexcept {
  if (dev == nullptr) {
    return false;
  }
  Retained& r = retained();
  const std::lock_guard<std::mutex> lock(r.mu);
  return r.devices.count(dev) != 0U;
}

GbmDevice::GbmDevice(struct gbm_device* dev) noexcept : dev_(dev) {}

GbmDevice::~GbmDevice() {
  if (dev_ != nullptr && !retained_for_egl(dev_)) {
    gbm_device_destroy(dev_);
  }
}

GbmDevice::GbmDevice(GbmDevice&& other) noexcept : dev_(other.dev_) {
  other.dev_ = nullptr;
}

GbmDevice& GbmDevice::operator=(GbmDevice&& other) noexcept {
  if (this != &other) {
    if (dev_ != nullptr && !retained_for_egl(dev_)) {
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
