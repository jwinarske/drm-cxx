// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <expected>
#include <system_error>

namespace drm::sync {

class SyncFence {
 public:
  static std::expected<SyncFence, std::error_code> import_fd(int fence_fd);

  std::expected<void, std::error_code> wait(std::chrono::milliseconds timeout);
  // Merge another fence into this one. The other fence is consumed (moved from).
  std::expected<void, std::error_code> merge(SyncFence other);

  ~SyncFence();
  SyncFence(SyncFence&&) noexcept;
  SyncFence& operator=(SyncFence&&) noexcept;
  SyncFence(const SyncFence&) = delete;
  SyncFence& operator=(const SyncFence&) = delete;

 private:
  explicit SyncFence(int fd) noexcept;
  int fd_{-1};
};

}  // namespace drm::sync
