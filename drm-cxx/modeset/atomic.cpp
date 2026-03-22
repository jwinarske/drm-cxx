// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "atomic.hpp"

#include "../core/device.hpp"

#include <xf86drm.h>

#include <cerrno>

namespace drm {

AtomicRequest::AtomicRequest(const Device& dev) : req_(drmModeAtomicAlloc()), drm_fd_(dev.fd()) {}

AtomicRequest::~AtomicRequest() {
  if (req_) {
    drmModeAtomicFree(req_);
  }
}

AtomicRequest::AtomicRequest(AtomicRequest&& other) noexcept
    : req_(other.req_), drm_fd_(other.drm_fd_) {
  other.req_ = nullptr;
  other.drm_fd_ = -1;
}

AtomicRequest& AtomicRequest::operator=(AtomicRequest&& other) noexcept {
  if (this != &other) {
    if (req_) {
      drmModeAtomicFree(req_);
    }
    req_ = other.req_;
    drm_fd_ = other.drm_fd_;
    other.req_ = nullptr;
    other.drm_fd_ = -1;
  }
  return *this;
}

std::expected<void, std::error_code> AtomicRequest::add_property(uint32_t object_id,
                                                                 uint32_t property_id,
                                                                 uint64_t value) {
  if (!req_) {
    return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int ret = drmModeAtomicAddProperty(req_, object_id, property_id, value);
  if (ret < 0) {
    return std::unexpected(std::error_code(-ret, std::system_category()));
  }
  return {};
}

std::expected<void, std::error_code> AtomicRequest::test(uint32_t flags) {
  if (!req_ || drm_fd_ < 0) {
    return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int ret = drmModeAtomicCommit(drm_fd_, req_, flags | DRM_MODE_ATOMIC_TEST_ONLY, nullptr);
  if (ret != 0) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }
  return {};
}

std::expected<void, std::error_code> AtomicRequest::commit(uint32_t flags, void* user_data) {
  if (!req_ || drm_fd_ < 0) {
    return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int ret = drmModeAtomicCommit(drm_fd_, req_, flags, user_data);
  if (ret != 0) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }
  return {};
}

}  // namespace drm
