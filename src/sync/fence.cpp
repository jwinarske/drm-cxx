// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "fence.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cerrno>
#include <chrono>
#include <limits>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <system_error>
#include <unistd.h>

namespace drm::sync {

SyncFence::SyncFence(int fd) noexcept : fd_(fd) {}

SyncFence::~SyncFence() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

SyncFence::SyncFence(SyncFence&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

SyncFence& SyncFence::operator=(SyncFence&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

drm::expected<SyncFence, std::error_code> SyncFence::import_fd(int fence_fd) {
  if (fence_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int const duped = ::dup(fence_fd);
  if (duped < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  return SyncFence(duped);
}

drm::expected<void, std::error_code> SyncFence::wait(std::chrono::milliseconds timeout) const {
  if (fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  struct pollfd pfd{};
  pfd.fd = fd_;
  pfd.events = POLLIN;

  auto ms = timeout.count();
  if (ms > std::numeric_limits<int>::max()) {
    ms = std::numeric_limits<int>::max();
  }
  int const ret = ::poll(&pfd, 1, static_cast<int>(ms));
  if (ret < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  if (ret == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::timed_out));
  }
  return {};
}

drm::expected<void, std::error_code> SyncFence::merge(SyncFence other) {
  if (fd_ < 0 || other.fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  struct sync_merge_data data{};
  data.fd2 = other.fd_;

  if (::ioctl(fd_, SYNC_IOC_MERGE, &data) < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  if (data.fence < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // Replace our fd with the merged one
  ::close(fd_);
  fd_ = data.fence;

  // other's destructor will close its fd
  return {};
}

}  // namespace drm::sync
