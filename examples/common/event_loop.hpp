// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// event_loop.hpp — small slot-based pollfd[] wrapper for the example
// main loops.
//
// Every example builds the same shape: a fixed pollfd array (input fd,
// drm fd, seat fd, plus 0–N optional extension fds), a poll() with a
// flip-pending-aware timeout, and a sequence of `if (revents & POLLIN)`
// handlers in registration order. This helper keeps the registration
// order explicit, lets the caller swap an fd in place (resume swaps
// the drm fd; the rear-view layer in cluster_sim toggles a UVC fd in
// and out), and centralises the EINTR + log-on-error path.
//
// What stays in the caller: the poll timeout itself (the example
// knows whether a flip is pending) and the post-poll work — pending
// resume, per-frame repaint + commit, etc. The loop only owns the
// fd/handler table and the poll() call.
//
// Header-only by intent, mirroring open_output.hpp / vt_switch.hpp.

#pragma once

#include <drm-cxx/detail/format.hpp>

#include <cerrno>
#include <cstddef>
#include <functional>
#include <poll.h>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::examples {

/// Slot-based wrapper around poll(). Each slot has a fd (which may be
/// `-1` to mean "currently inactive — skip this slot") and a handler
/// that runs when the fd is POLLIN-ready. Slots are dispatched in
/// registration order.
class EventLoop {
 public:
  using Handler = std::function<void()>;

  /// Register a slot. `fd == -1` is fine and means the slot is
  /// inactive until `set_fd` activates it. Returns the slot index;
  /// indices are assigned sequentially from 0.
  int add_slot(int fd, Handler handler) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    fds_.push_back(pfd);
    handlers_.push_back(std::move(handler));
    return static_cast<int>(fds_.size()) - 1;
  }

  /// Update a slot's fd. Used after session resume (the drm fd
  /// changes) and for "optional" slots that turn on/off at runtime
  /// (cluster_sim's UVC rear-view fd, video_player's gst sample
  /// queue eventfd, etc.).
  void set_fd(int slot, int fd) noexcept { fds_.at(static_cast<std::size_t>(slot)).fd = fd; }

  /// One poll() iteration. `timeout_ms` follows poll(2) semantics:
  /// negative blocks indefinitely, zero polls non-blocking, positive
  /// is the millisecond timeout. Handlers fire in registration order
  /// for slots with POLLIN ready.
  ///
  /// Returns true on a normal poll() return (including EINTR — no
  /// handlers fire, but the caller's main loop should keep going).
  /// Returns false on a hard poll() error; the caller's main loop
  /// should break out. Errors are logged to stderr before returning.
  [[nodiscard]] bool tick(int timeout_ms) {
    int const ret = ::poll(fds_.data(), fds_.size(), timeout_ms);
    if (ret < 0) {
      if (errno == EINTR) {
        return true;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      return false;
    }
    if (ret == 0) {
      return true;
    }
    for (std::size_t i = 0; i < fds_.size(); ++i) {
      if ((fds_[i].revents & POLLIN) != 0 && handlers_[i]) {
        handlers_[i]();
      }
    }
    return true;
  }

 private:
  std::vector<pollfd> fds_;
  std::vector<Handler> handlers_;
};

}  // namespace drm::examples