// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "fence.hpp"

namespace drm::sync {

SyncFence::SyncFence(int fd) noexcept : fd_(fd) {}
SyncFence::~SyncFence() = default;

SyncFence::SyncFence(SyncFence&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

SyncFence& SyncFence::operator=(SyncFence&& other) noexcept {
  if (this != &other) {
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

std::expected<SyncFence, std::error_code>
SyncFence::import_fd([[maybe_unused]] int fence_fd) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

std::expected<void, std::error_code>
SyncFence::wait([[maybe_unused]] std::chrono::milliseconds timeout) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

void SyncFence::merge([[maybe_unused]] SyncFence& other) {}

} // namespace drm::sync
