// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <system_error>

struct gbm_bo;

namespace drm::gbm {

class Buffer {
public:
  [[nodiscard]] struct gbm_bo* raw() const noexcept;
  [[nodiscard]] uint32_t handle() const noexcept;
  [[nodiscard]] uint32_t stride() const noexcept;

  ~Buffer();
  Buffer(Buffer&&) noexcept;
  Buffer& operator=(Buffer&&) noexcept;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

private:
  explicit Buffer(struct gbm_bo* bo) noexcept;
  struct gbm_bo* bo_{};
  friend class Surface;
};

} // namespace drm::gbm
