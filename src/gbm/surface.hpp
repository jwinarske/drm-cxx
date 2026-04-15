// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "buffer.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <system_error>

struct gbm_surface;

namespace drm::gbm {

class GbmDevice;

class Surface {
 public:
  static drm::expected<Surface, std::error_code> create(GbmDevice& dev, uint32_t width,
                                                        uint32_t height, uint32_t format,
                                                        uint32_t flags);

  [[nodiscard]] struct gbm_surface* raw() const noexcept;

  // Lock the front buffer after an EGL swap. Returns a Buffer RAII wrapper.
  drm::expected<Buffer, std::error_code> lock_front_buffer();

  // Check if the surface has a free buffer available.
  [[nodiscard]] bool has_free_buffers() const noexcept;

  ~Surface();
  Surface(Surface&& /*other*/) noexcept;
  Surface& operator=(Surface&& /*other*/) noexcept;
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

 private:
  explicit Surface(struct gbm_surface* surf) noexcept;
  struct gbm_surface* surf_{};
};

}  // namespace drm::gbm
