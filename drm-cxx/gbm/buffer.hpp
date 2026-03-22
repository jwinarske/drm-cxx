// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <system_error>

struct gbm_bo;
struct gbm_surface;

namespace drm::gbm {

class Buffer {
public:
  [[nodiscard]] struct gbm_bo* raw() const noexcept;
  [[nodiscard]] uint32_t handle() const noexcept;
  [[nodiscard]] uint32_t stride() const noexcept;
  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;
  [[nodiscard]] uint32_t format() const noexcept;

  // Get a DMA-BUF fd for this buffer.
  [[nodiscard]] std::expected<int, std::error_code> fd() const;

  ~Buffer();
  Buffer(Buffer&&) noexcept;
  Buffer& operator=(Buffer&&) noexcept;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

private:
  explicit Buffer(struct gbm_bo* bo, struct gbm_surface* surf = nullptr) noexcept;
  struct gbm_bo* bo_{};
  struct gbm_surface* surf_{};  // Non-null if locked from a surface
  friend class Surface;
};

} // namespace drm::gbm
