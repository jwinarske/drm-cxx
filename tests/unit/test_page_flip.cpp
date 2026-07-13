// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::PageFlip's foreign-source registration.
//
// Covers the contract reachable without a live KMS device:
// add_source / remove_source argument validation, the eventfd
// readiness round-trip, idempotent removal, and move semantics.
//
// The page-flip-event side of dispatch() needs a real DRM fd and is
// covered by the example suites (scene_warm_start, atomic_modeset,
// scene_priority, etc.) plus the VKMS integration tests; this TU
// exercises only the foreign-source half so it stays green on every
// CI runner.

#include "core/device.hpp"
#include "modeset/page_flip.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <gtest/gtest.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX sigaction/setitimer live here, not <csignal>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

// Open a real DRM device for the PageFlip ctor. PageFlip's ctor takes
// a Device and reads its fd; with `Device::from_fd(-1)` the persistent
// epfd_ still gets created but adding a foreign fd on top is enough
// to exercise add_source / remove_source / dispatch's foreign-fd
// branch — no DRM ioctls fire on that path.
drm::Device make_device_with_fd(int fd) {
  return drm::Device::from_fd(fd);
}

// Count of SIGALRM deliveries, so a test can assert the storm actually
// fired (otherwise a "passing" EINTR test may not have exercised EINTR).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_alrm_count = 0;

// A fd the handler makes readable after k_ready_after_ticks interruptions,
// or -1 to keep the storm purely interrupting (no readiness). write() is
// async-signal-safe, so ending the storm from the handler itself lets one
// thread both generate the EINTR storm and terminate it — no second thread
// or signal masking to steer delivery.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int g_ready_fd = -1;

constexpr int k_ready_after_ticks = 20;

void on_sigalrm(int /*signo*/) {
  const int ticks = ++g_alrm_count;
  if (ticks == k_ready_after_ticks && g_ready_fd >= 0) {
    const std::uint64_t bump = 1;
    const ssize_t written = ::write(g_ready_fd, &bump, sizeof(bump));
    (void)written;
  }
}

// RAII: install a deliberately non-restarting SIGALRM handler and arm a
// 1 ms interval timer, so any epoll_wait running while this is alive is
// interrupted (EINTR) roughly every millisecond. Restores the previous
// handler and disarms the timer on scope exit.
class SigalrmStorm {
 public:
  SigalrmStorm() {
    g_alrm_count = 0;
    struct sigaction sa{};
    sa.sa_handler = on_sigalrm;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: interrupted syscalls return EINTR.
    ::sigaction(SIGALRM, &sa, &old_sa_);
    struct itimerval it{};
    it.it_interval.tv_usec = 1000;  // 1 ms cadence
    it.it_value.tv_usec = 1000;
    ::setitimer(ITIMER_REAL, &it, &old_it_);
  }
  ~SigalrmStorm() {
    const struct itimerval off{};
    ::setitimer(ITIMER_REAL, &off, nullptr);
    ::sigaction(SIGALRM, &old_sa_, nullptr);
  }
  SigalrmStorm(const SigalrmStorm&) = delete;
  SigalrmStorm& operator=(const SigalrmStorm&) = delete;
  SigalrmStorm(SigalrmStorm&&) = delete;
  SigalrmStorm& operator=(SigalrmStorm&&) = delete;

 private:
  struct sigaction old_sa_{};
  struct itimerval old_it_{};
};

}  // namespace

TEST(PageFlipForeignSource, AddSourceRejectsNegativeFd) {
  auto dev = make_device_with_fd(-1);
  drm::PageFlip pf(dev);
  auto r = pf.add_source(-1, [] {});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(PageFlipForeignSource, AddSourceRejectsDuplicateFd) {
  // Use a real fd that epoll will accept — the duplicate check needs
  // the first add_source to succeed.
  const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    GTEST_SKIP() << "eventfd unavailable";
  }
  auto dev = make_device_with_fd(-1);
  drm::PageFlip pf(dev);
  auto first = pf.add_source(efd, [] {});
  ASSERT_TRUE(first.has_value());
  auto second = pf.add_source(efd, [] {});
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error(), std::make_error_code(std::errc::invalid_argument));
  pf.remove_source(efd);
  ::close(efd);
}

TEST(PageFlipForeignSource, RemoveSourceIsIdempotent) {
  const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    GTEST_SKIP() << "eventfd unavailable";
  }
  auto dev = make_device_with_fd(-1);
  drm::PageFlip pf(dev);
  // Remove of never-added: no-op.
  pf.remove_source(efd);
  // Add then remove twice: the first removes, the second is a no-op.
  ASSERT_TRUE(pf.add_source(efd, [] {}).has_value());
  pf.remove_source(efd);
  pf.remove_source(efd);
  ::close(efd);
}

TEST(PageFlipForeignSource, EventfdReadinessFiresCallback) {
  const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    GTEST_SKIP() << "eventfd unavailable";
  }
  auto dev = make_device_with_fd(-1);
  drm::PageFlip pf(dev);
  int calls = 0;
  ASSERT_TRUE(pf.add_source(efd, [&] {
                  // Drain the eventfd so the next dispatch doesn't
                  // re-fire on the same readiness.
                  std::uint64_t val = 0;
                  ::read(efd, &val, sizeof(val));
                  ++calls;
                }).has_value());

  // Make the eventfd readable.
  const std::uint64_t bump = 1;
  ASSERT_EQ(::write(efd, &bump, sizeof(bump)), static_cast<ssize_t>(sizeof(bump)));

  // Non-blocking dispatch — the eventfd is readable, the callback
  // must run, and dispatch must return success.
  auto r = pf.dispatch(0);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_EQ(calls, 1);

  // Second dispatch with no readiness times out and the callback
  // doesn't re-fire.
  auto r2 = pf.dispatch(0);
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error(), std::make_error_code(std::errc::timed_out));
  EXPECT_EQ(calls, 1);

  pf.remove_source(efd);
  ::close(efd);
}

TEST(PageFlipForeignSource, DispatchTimesOutWithNoReadyFds) {
  const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    GTEST_SKIP() << "eventfd unavailable";
  }
  auto dev = make_device_with_fd(-1);
  drm::PageFlip pf(dev);
  ASSERT_TRUE(pf.add_source(efd, [] {}).has_value());
  // No write to efd — dispatch with a 0ms timeout must report
  // timed_out, not bad_file_descriptor.
  auto r = pf.dispatch(0);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::timed_out));
  pf.remove_source(efd);
  ::close(efd);
}

TEST(PageFlipForeignSource, MoveTransfersDispatcher) {
  const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    GTEST_SKIP() << "eventfd unavailable";
  }
  auto dev = make_device_with_fd(-1);
  drm::PageFlip pf(dev);
  int calls = 0;
  ASSERT_TRUE(pf.add_source(efd, [&] {
                  std::uint64_t val = 0;
                  ::read(efd, &val, sizeof(val));
                  ++calls;
                }).has_value());

  drm::PageFlip moved = std::move(pf);

  const std::uint64_t bump = 1;
  ASSERT_EQ(::write(efd, &bump, sizeof(bump)), static_cast<ssize_t>(sizeof(bump)));

  // The moved-to instance owns the registration now and must fire
  // the callback.
  auto r = moved.dispatch(0);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_EQ(calls, 1);

  moved.remove_source(efd);
  ::close(efd);
}

TEST(PageFlipRebind, RejectsInvalidNewFd) {
  auto dev = make_device_with_fd(-1);
  drm::PageFlip pf(dev);
  auto new_dev = make_device_with_fd(-1);
  auto r = pf.rebind(new_dev);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::bad_file_descriptor));
}

TEST(PageFlipRebind, SwapsDrmFdRegistration) {
  // Stand in for a real DRM fd with a plain eventfd. PageFlip's
  // dispatcher cares only that the fd is epoll-addable and EPOLLIN-
  // readable for the registration / removal paths. We deliberately
  // don't push events on these — drmHandleEvent against eventfd
  // payload would parse garbage as a drm_event header and hang.
  const int first = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  const int second = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (first < 0 || second < 0) {
    if (first >= 0) {
      ::close(first);
    }
    if (second >= 0) {
      ::close(second);
    }
    GTEST_SKIP() << "eventfd unavailable";
  }
  auto first_dev = drm::Device::from_fd(first);
  drm::PageFlip pf(first_dev);

  // dispatch(0) on no-ready-fds returns timed_out — confirms the
  // initial registration is live.
  auto initial = pf.dispatch(0);
  ASSERT_FALSE(initial.has_value());
  EXPECT_EQ(initial.error(), std::make_error_code(std::errc::timed_out));

  auto second_dev = drm::Device::from_fd(second);
  auto r = pf.rebind(second_dev);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  // After rebind the second fd is the live registration. dispatch(0)
  // still returns timed_out (no events queued) — that's the success
  // signal: the epoll set isn't half-registered. A failed rebind that
  // left the kernel without any source registered would return
  // bad_file_descriptor here instead.
  auto after = pf.dispatch(0);
  ASSERT_FALSE(after.has_value());
  EXPECT_EQ(after.error(), std::make_error_code(std::errc::timed_out));

  ::close(first);
  ::close(second);
}

// dispatch(-1) under a 1 ms SIGALRM storm must swallow every EINTR and
// return only once a source is actually readable — never
// errc::interrupted. Without the internal retry, the first interrupting
// signal (~1 ms in) would abandon the wait and surface interrupted,
// dropping a flip still in flight.
TEST(PageFlipEintr, DispatchInfiniteRetriesUntilReady) {
  const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    GTEST_SKIP() << "eventfd unavailable";
  }
  auto dev = make_device_with_fd(-1);
  drm::PageFlip pf(dev);
  int calls = 0;
  ASSERT_TRUE(pf.add_source(efd, [&] {
                  std::uint64_t val = 0;
                  ::read(efd, &val, sizeof(val));
                  ++calls;
                }).has_value());

  // The handler makes efd readable only after k_ready_after_ticks
  // interruptions, so dispatch(-1) blocks, is interrupted many times, and
  // must swallow every EINTR — returning success once efd is readable,
  // never errc::interrupted.
  g_ready_fd = efd;
  {
    const SigalrmStorm storm;
    auto r = pf.dispatch(-1);
    EXPECT_TRUE(r.has_value()) << r.error().message();
  }
  g_ready_fd = -1;

  EXPECT_EQ(calls, 1);
  EXPECT_GE(g_alrm_count, k_ready_after_ticks) << "storm never fired — EINTR path untested";

  pf.remove_source(efd);
  ::close(efd);
}

// dispatch(timeout_ms > 0) under the same storm must still complete at
// ~timeout_ms, not restart the full budget per signal. A naive retry that
// re-passed timeout_ms to epoll_wait each time would never terminate under
// a 1 ms storm; recomputing the remaining budget from a steady deadline
// bounds the total wait.
TEST(PageFlipEintr, DispatchBoundedTimeoutHonorsBudgetUnderStorm) {
  const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    GTEST_SKIP() << "eventfd unavailable";
  }
  auto dev = make_device_with_fd(-1);
  drm::PageFlip pf(dev);
  ASSERT_TRUE(pf.add_source(efd, [] {}).has_value());  // never made readable

  const SigalrmStorm storm;
  const auto start = std::chrono::steady_clock::now();
  auto r = pf.dispatch(50);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();

  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::timed_out));
  EXPECT_GE(elapsed_ms, 45) << "returned before the budget elapsed";
  EXPECT_LT(elapsed_ms, 500) << "budget not recomputed across EINTR retries";
  EXPECT_GT(g_alrm_count, 0) << "storm never fired — EINTR path untested";

  pf.remove_source(efd);
  ::close(efd);
}