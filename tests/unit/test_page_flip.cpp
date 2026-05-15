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

#include <cstdint>
#include <gtest/gtest.h>
#include <sys/eventfd.h>
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