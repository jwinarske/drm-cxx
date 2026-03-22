// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "buffer.hpp"

namespace drm::gbm {

Buffer::Buffer(struct gbm_bo* bo) noexcept : bo_(bo) {}
Buffer::~Buffer() = default;

Buffer::Buffer(Buffer&& other) noexcept : bo_(other.bo_) {
  other.bo_ = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    bo_ = other.bo_;
    other.bo_ = nullptr;
  }
  return *this;
}

struct gbm_bo* Buffer::raw() const noexcept { return bo_; }
uint32_t Buffer::handle() const noexcept { return 0; }
uint32_t Buffer::stride() const noexcept { return 0; }

} // namespace drm::gbm
