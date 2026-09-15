// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "atomic.hpp"

#include "../core/device.hpp"
#include "log.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <utility>

namespace drm {

namespace {
// One env check for the whole module. DRM_ALLOC_DEBUG also enables it, so the
// allocator's own trace and the request contents come out together -- reading
// one without the other is what makes an EINVAL hard to place.
bool atomic_debug() {
  static const bool enabled =
      std::getenv("DRM_ATOMIC_DEBUG") != nullptr || std::getenv("DRM_ALLOC_DEBUG") != nullptr;
  return enabled;
}
}  // namespace

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
    : req_(other.req_), drm_fd_(other.drm_fd_), trace_(std::move(other.trace_)) {
  other.req_ = nullptr;
  other.drm_fd_ = -1;
  other.trace_.clear();
}

AtomicRequest& AtomicRequest::operator=(AtomicRequest&& other) noexcept {
  if (this != &other) {
    if (req_ != nullptr) {
      drmModeAtomicFree(req_);
    }
    req_ = other.req_;
    drm_fd_ = other.drm_fd_;
    trace_ = std::move(other.trace_);
    other.req_ = nullptr;
    other.drm_fd_ = -1;
    other.trace_.clear();
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
  if (atomic_debug()) {
    trace_.push_back(Entry{object_id, property_id, value});
  }
  return {};
}

void AtomicRequest::dump(const char* why) const {
  if (!atomic_debug()) {
    return;
  }
  // Routed through log_channel rather than log_debug: this is opt-in via an
  // env var, so the global level should not get a second veto -- the same
  // reasoning as the allocator trace this accompanies.
  drm::detail::log_channel(drm::LogLevel::Debug, "[atomic] {}: {} propert{} in this request", why,
                           trace_.size(), trace_.size() == 1 ? "y" : "ies");
  for (const Entry& e : trace_) {
    const char* name = "?";
    drmModePropertyRes* pr = drm_fd_ >= 0 ? drmModeGetProperty(drm_fd_, e.property_id) : nullptr;
    if (pr != nullptr) {
      name = pr->name;
    }
    drm::detail::log_channel(drm::LogLevel::Debug, "[atomic]   obj={:<4} {:<24} = {}", e.object_id,
                             name, e.value);
    if (pr != nullptr) {
      drmModeFreeProperty(pr);
    }
  }
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
