// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// examples/common/quit_signal.hpp — Ctrl-C / SIGTERM handling that survives
// GPU stacks which take the signals over.
//
// Some EGL implementations install their own SIGINT/SIGTERM handlers the first
// time EGL is used, replacing the application's (Vivante's libEGL does, as a
// one-shot handler that tears its state down and lets the signal kill the
// process — so the example's own teardown never runs). EGL can come up inside
// the library too, e.g. when a scene falls back to GPU composition, so a handler
// installed before that point is not enough.
//
// route_quit_signals() blocks SIGINT and SIGTERM instead, so no asynchronous
// handler ever runs, and a dedicated thread sigwait()s for them and calls
// `handler`. Call it first thing in main(), before any other thread exists:
// threads inherit the blocked mask, and one created earlier could still take
// the signal. `handler` runs on that thread, so it should only set an atomic
// flag.
//
// A blocked signal does not interrupt a blocking syscall (no EINTR), so a loop
// that may wait indefinitely (poll / EventLoop::tick with a negative timeout)
// adds quit_wake_fd() to the set it waits on: it becomes readable once
// `handler` has run. It is never drained — the loop is expected to exit.

#include <signal.h>  // NOLINT(modernize-deprecated-headers) -- POSIX sigset_t/sigwait/pthread_sigmask
#include <sys/eventfd.h>
#include <thread>

namespace drm::examples {

namespace detail {
inline int quit_wake_fd_storage = -1;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}  // namespace detail

inline void route_quit_signals(void (*handler)(int)) {
  sigset_t set;  // NOLINT(misc-include-cleaner) -- POSIX type from <signal.h>
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  const int wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  detail::quit_wake_fd_storage = wake;
  std::thread([set, handler, wake] {
    for (;;) {
      int sig = 0;
      if (sigwait(&set, &sig) == 0) {
        handler(sig);
        if (wake >= 0) {
          (void)::eventfd_write(wake, 1);
        }
      }
    }
  }).detach();
}

// Readable after a quit signal has been handled; -1 before route_quit_signals()
// (or if the eventfd could not be created — poll() ignores a negative fd).
[[nodiscard]] inline int quit_wake_fd() noexcept {
  return detail::quit_wake_fd_storage;
}

}  // namespace drm::examples
