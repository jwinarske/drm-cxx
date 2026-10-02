// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// examples/common/quit_signal.hpp — Ctrl-C / SIGTERM handling that survives
// GPU stacks which take the signals over.
//
// Some EGL implementations install their own SIGINT/SIGTERM handlers the first
// time EGL is used, replacing the application's (Vivante's libEGL does, as a
// one-shot handler that tears its state down and lets the signal kill the
// process — so the example's own teardown never runs). A handler installed
// before EGL comes up is therefore not enough.
//
// route_quit_signals() blocks SIGINT and SIGTERM instead, so no asynchronous
// handler ever runs, and a dedicated thread sigwait()s for them and calls
// `handler`. Call it first thing in main(), before any other thread exists:
// threads inherit the blocked mask, and one created earlier could still take
// the signal. `handler` runs on that thread, so it should only set an atomic
// flag. A blocked signal does not interrupt a blocking syscall (no EINTR), so
// the loop that checks the flag must wake up on its own (poll with a timeout).

#include <csignal>
#include <pthread.h>
#include <thread>

namespace drm::examples {

inline void route_quit_signals(void (*handler)(int)) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  std::thread([set, handler] {
    for (;;) {
      int sig = 0;
      if (sigwait(&set, &sig) == 0) {
        handler(sig);
      }
    }
  }).detach();
}

}  // namespace drm::examples
