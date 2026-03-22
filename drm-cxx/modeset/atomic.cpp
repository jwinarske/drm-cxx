// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "atomic.hpp"
#include "../core/device.hpp"

namespace drm {

AtomicRequest::AtomicRequest(const Device& dev) : drm_fd_(dev.fd()) {}

AtomicRequest::~AtomicRequest() = default;

AtomicRequest::AtomicRequest(AtomicRequest&& other) noexcept
  : req_(other.req_), drm_fd_(other.drm_fd_) {
  other.req_ = nullptr;
  other.drm_fd_ = -1;
}

AtomicRequest& AtomicRequest::operator=(AtomicRequest&& other) noexcept {
  if (this != &other) {
    req_ = other.req_;
    drm_fd_ = other.drm_fd_;
    other.req_ = nullptr;
    other.drm_fd_ = -1;
  }
  return *this;
}

std::expected<void, std::error_code>
AtomicRequest::add_property([[maybe_unused]] uint32_t object_id,
                            [[maybe_unused]] uint32_t property_id,
                            [[maybe_unused]] uint64_t value) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

std::expected<void, std::error_code>
AtomicRequest::test([[maybe_unused]] uint32_t flags) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

std::expected<void, std::error_code>
AtomicRequest::commit([[maybe_unused]] uint32_t flags,
                      [[maybe_unused]] void* user_data) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace drm
