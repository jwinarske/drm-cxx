// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <functional>
#include <system_error>
#include <unordered_map>

namespace drm {

class Device;

class PageFlip {
 public:
  using Handler = std::function<void(uint32_t crtc_id, uint64_t sequence, uint64_t timestamp_ns)>;

  /// Callback fired by `dispatch()` when a fd registered via
  /// `add_source` becomes readable. PageFlip never reads from `fd`
  /// itself — the callback is responsible for draining it (eventfd_read,
  /// signalfd_siginfo, recvmsg, ...) before returning, otherwise the
  /// next dispatch will fire the same callback again.
  using SourceCallback = std::function<void()>;

  explicit PageFlip(const Device& dev);

  void set_handler(Handler handler);

  /// Register a foreign fd on this PageFlip's dispatcher. Useful for
  /// folding external event sources (libcamera completion eventfd,
  /// signalfd for SIGINT, udev monitor) into the same dispatch loop
  /// the page-flip events ride on, so callers don't need to run a
  /// parallel epoll alongside `dispatch()`.
  ///
  /// PageFlip retains *no* ownership of `fd` — the caller closes it.
  /// `remove_source(fd)` must run (or this PageFlip must be destroyed)
  /// before the fd is closed; otherwise the kernel will keep firing
  /// EPOLLERR on the stale registration.
  ///
  /// Returns `invalid_argument` for a negative fd or a duplicate
  /// (already-registered) fd.
  [[nodiscard]] drm::expected<void, std::error_code> add_source(int fd, SourceCallback cb);

  /// Unregister a previously-added fd. Idempotent — calling on a
  /// never-added fd is a no-op, and calling twice is safe.
  void remove_source(int fd) noexcept;

  // Wait for and dispatch a page flip event.
  // timeout_ms: -1 = block forever, 0 = non-blocking, >0 = timeout in ms.
  //
  // When foreign sources have been added, dispatch wakes on either the
  // DRM fd or any foreign fd; each ready event routes to the
  // page-flip handler or the foreign source's callback respectively.
  // Returns `timed_out` if `timeout_ms` elapses with no events.
  //
  // Single-threaded contract: do not call dispatch / add_source /
  // remove_source concurrently against the same instance.
  [[nodiscard]] drm::expected<void, std::error_code> dispatch(int timeout_ms = -1);

  ~PageFlip();
  PageFlip(PageFlip&& other) noexcept;
  PageFlip& operator=(PageFlip&& other) noexcept;
  PageFlip(const PageFlip&) = delete;
  PageFlip& operator=(const PageFlip&) = delete;

 private:
  // Allow the C callback trampolines to invoke handler_
  friend void page_flip_handler(int /*unused*/, unsigned int /*unused*/, unsigned int /*tv_sec*/,
                                unsigned int /*tv_usec*/, void* /*user_data*/);
  friend void page_flip_handler_v2(int /*unused*/, unsigned int /*sequence*/,
                                   unsigned int /*tv_sec*/, unsigned int /*tv_usec*/,
                                   unsigned int /*crtc_id*/, void* /*user_data*/);

  void close_epfd() noexcept;

  int drm_fd_{-1};
  int epfd_{-1};
  Handler handler_;
  std::unordered_map<int, SourceCallback> sources_;
};

}  // namespace drm
