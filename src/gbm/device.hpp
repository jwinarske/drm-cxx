// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <system_error>

struct gbm_device;

namespace drm::gbm {

// Some EGL implementations keep every gbm_device handed to
// eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, ...) and destroy it themselves at
// process exit, without unregistering it on eglTerminate (Vivante's libEGL
// does). Destroying such a device earlier turns that exit handler into a
// use-after-free. Call retain_for_egl() after eglInitialize on a display made
// over `dev`, with that display's EGL_VENDOR string: when the vendor is one that
// does this, `dev` is recorded and returns true, GbmDevice then leaves it
// alive, and code that destroys gbm devices itself should check
// retained_for_egl() first. Safe to call from any thread.
bool retain_for_egl(struct gbm_device* dev, const char* egl_vendor) noexcept;
[[nodiscard]] bool retained_for_egl(const struct gbm_device* dev) noexcept;

class GbmDevice {
 public:
  static drm::expected<GbmDevice, std::error_code> create(int drm_fd);

  [[nodiscard]] struct gbm_device* raw() const noexcept;

  ~GbmDevice();
  GbmDevice(GbmDevice&& /*other*/) noexcept;
  GbmDevice& operator=(GbmDevice&& /*other*/) noexcept;
  GbmDevice(const GbmDevice&) = delete;
  GbmDevice& operator=(const GbmDevice&) = delete;

 private:
  explicit GbmDevice(struct gbm_device* dev) noexcept;
  struct gbm_device* dev_{};
};

}  // namespace drm::gbm
