// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <system_error>

struct gbm_surface;

namespace drm::gbm {

class GbmDevice;

class Surface {
public:
  static std::expected<Surface, std::error_code>
    create(GbmDevice& dev, uint32_t width, uint32_t height,
           uint32_t format, uint32_t flags);

  [[nodiscard]] struct gbm_surface* raw() const noexcept;

  ~Surface();
  Surface(Surface&&) noexcept;
  Surface& operator=(Surface&&) noexcept;
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

private:
  explicit Surface(struct gbm_surface* surf) noexcept;
  struct gbm_surface* surf_{};
};

} // namespace drm::gbm
