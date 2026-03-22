// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "fence.hpp"

#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>

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

std::expected<SyncFence, std::error_code>
SyncFence::import_fd(int fence_fd) {
  if (fence_fd < 0) {
    return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }
  int duped = ::dup(fence_fd);
  if (duped < 0) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }
  return SyncFence(duped);
}

std::expected<void, std::error_code>
SyncFence::wait(std::chrono::milliseconds timeout) {
  if (fd_ < 0) {
    return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
  }

  struct pollfd pfd{};
  pfd.fd = fd_;
  pfd.events = POLLIN;

  int ret = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
  if (ret < 0) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }
  if (ret == 0) {
    return std::unexpected(std::make_error_code(std::errc::timed_out));
  }
  return {};
}

void SyncFence::merge(SyncFence& other) {
  if (fd_ < 0 || other.fd_ < 0) {
    return;
  }

  struct sync_merge_data data{};
  data.fd2 = other.fd_;
  // name is a char array, leave as zeros

  if (::ioctl(fd_, SYNC_IOC_MERGE, &data) < 0) {
    return;
  }

  // Replace our fd with the merged one
  ::close(fd_);
  fd_ = data.fence;

  // Close the other fence
  ::close(other.fd_);
  other.fd_ = -1;
}

} // namespace drm::sync
