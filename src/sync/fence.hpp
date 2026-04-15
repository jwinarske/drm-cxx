// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <chrono>
#include <system_error>

namespace drm::sync {

class SyncFence {
 public:
  static drm::expected<SyncFence, std::error_code> import_fd(int fence_fd);

  [[nodiscard]] drm::expected<void, std::error_code> wait(std::chrono::milliseconds timeout) const;
  // Merge another fence into this one. The other fence is consumed (moved from).
  [[nodiscard]] drm::expected<void, std::error_code> merge(SyncFence other);

  ~SyncFence();
  SyncFence(SyncFence&& /*other*/) noexcept;
  SyncFence& operator=(SyncFence&& /*other*/) noexcept;
  SyncFence(const SyncFence&) = delete;
  SyncFence& operator=(const SyncFence&) = delete;

 private:
  explicit SyncFence(int fd) noexcept;
  int fd_{-1};
};

}  // namespace drm::sync
