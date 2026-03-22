// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <system_error>

struct gbm_device;

namespace drm::gbm {

class GbmDevice {
 public:
  static std::expected<GbmDevice, std::error_code> create(int drm_fd);

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
