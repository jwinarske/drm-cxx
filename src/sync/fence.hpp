// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <chrono>
#include <system_error>

namespace drm::sync {

class SyncFence {
 public:
  // Empty fence (no underlying fd; valid() == false). Lets a caller
  // default-construct an out-parameter that commit()/present() fill in.
  SyncFence() noexcept = default;

  static drm::expected<SyncFence, std::error_code> import_fd(int fence_fd);

  // True once this holds a real sync_file fd (e.g. a commit wrote an OUT_FENCE).
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  [[nodiscard]] drm::expected<void, std::error_code> wait(std::chrono::milliseconds timeout) const;
  // Merge another fence into this one. The other fence is consumed (moved from).
  [[nodiscard]] drm::expected<void, std::error_code> merge(SyncFence other);

  // The underlying sync_file fd, for passing to a plane's IN_FENCE_FD property.
  // The SyncFence keeps ownership: the kernel does not close IN_FENCE_FD, so the
  // fence still closes the fd on destruction. Returns -1 once moved-from.
  [[nodiscard]] int fd() const noexcept { return fd_; }

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
