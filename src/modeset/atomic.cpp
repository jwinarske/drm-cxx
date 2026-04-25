// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "atomic.hpp"

#include "../core/device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <system_error>

namespace drm {

AtomicRequest::AtomicRequest(const Device& dev) : req_(drmModeAtomicAlloc()), drm_fd_(dev.fd()) {}

bool AtomicRequest::valid() const noexcept {
  return req_ != nullptr && drm_fd_ >= 0;
}

AtomicRequest::~AtomicRequest() {
  if (req_ != nullptr) {
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
    if (req_ != nullptr) {
      drmModeAtomicFree(req_);
    }
    req_ = other.req_;
    drm_fd_ = other.drm_fd_;
    other.req_ = nullptr;
    other.drm_fd_ = -1;
  }
  return *this;
}

drm::expected<void, std::error_code> AtomicRequest::add_property(uint32_t object_id,
                                                                 uint32_t property_id,
                                                                 uint64_t value) {
  if (req_ == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int const ret = drmModeAtomicAddProperty(req_, object_id, property_id, value);
  if (ret < 0) {
    return drm::unexpected<std::error_code>(std::error_code(-ret, std::system_category()));
  }
  return {};
}

drm::expected<void, std::error_code> AtomicRequest::test(uint32_t flags) {
  if ((req_ == nullptr) || drm_fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  // DRM_MODE_ATOMIC_TEST_ONLY + DRM_MODE_PAGE_FLIP_EVENT is rejected by
  // drm_mode_atomic_ioctl with EINVAL — TEST doesn't apply state to
  // hardware, so there's no flip for the kernel to queue an event on.
  // Callers routinely forward the same flags they'd pass to commit()
  // (e.g. the plane allocator replays the caller's flags for its
  // internal TEST commits), so mask the event bit here rather than
  // making every caller remember.
  const uint32_t test_flags =
      (flags & ~static_cast<uint32_t>(DRM_MODE_PAGE_FLIP_EVENT)) | DRM_MODE_ATOMIC_TEST_ONLY;
  int const ret = drmModeAtomicCommit(drm_fd_, req_, test_flags, nullptr);
  if (ret != 0) {
    int const err = (ret < 0) ? -ret : errno;
    return drm::unexpected<std::error_code>(std::error_code(err, std::system_category()));
  }
  return {};
}

drm::expected<void, std::error_code> AtomicRequest::commit(uint32_t flags, void* user_data) {
  if ((req_ == nullptr) || drm_fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int const ret = drmModeAtomicCommit(drm_fd_, req_, flags, user_data);
  if (ret != 0) {
    int const err = (ret < 0) ? -ret : errno;
    return drm::unexpected<std::error_code>(std::error_code(err, std::system_category()));
  }
  return {};
}

}  // namespace drm
