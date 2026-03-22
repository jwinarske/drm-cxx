// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "sync/fence.hpp"

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

TEST(SyncFenceTest, ImportInvalidFdReturnsError) {
  auto result = drm::sync::SyncFence::import_fd(-1);
  ASSERT_FALSE(result.has_value());
}

TEST(SyncFenceTest, ImportValidFdSucceeds) {
  // Use an eventfd as a stand-in for a real sync fence fd
  int const efd = eventfd(0, EFD_CLOEXEC);
  ASSERT_GE(efd, 0);

  auto result = drm::sync::SyncFence::import_fd(efd);
  ASSERT_TRUE(result.has_value());

  // The import should have dup'd the fd, so we can close the original
  ::close(efd);
  // The SyncFence destructor will close the duped fd
}

TEST(SyncFenceTest, MoveConstructor) {
  int const efd = eventfd(0, EFD_CLOEXEC);
  ASSERT_GE(efd, 0);

  auto result = drm::sync::SyncFence::import_fd(efd);
  ASSERT_TRUE(result.has_value());
  ::close(efd);

  drm::sync::SyncFence const fence2(std::move(*result));
  // fence2 should hold the fd now; original is moved-from
}

TEST(SyncFenceTest, MoveAssignment) {
  int const efd1 = eventfd(0, EFD_CLOEXEC);
  int const efd2 = eventfd(0, EFD_CLOEXEC);
  ASSERT_GE(efd1, 0);
  ASSERT_GE(efd2, 0);

  auto r1 = drm::sync::SyncFence::import_fd(efd1);
  auto r2 = drm::sync::SyncFence::import_fd(efd2);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  ::close(efd1);
  ::close(efd2);

  *r1 = std::move(*r2);
  // r1 now holds efd2's duped fd; efd1's duped fd was closed
}

TEST(SyncFenceTest, WaitOnSignaledEventFdSucceeds) {
  int const efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(efd, 0);

  // Signal the eventfd so poll returns immediately
  uint64_t val = 1;
  ASSERT_EQ(::write(efd, &val, sizeof(val)), static_cast<ssize_t>(sizeof(val)));

  auto result = drm::sync::SyncFence::import_fd(efd);
  ASSERT_TRUE(result.has_value());
  ::close(efd);

  auto wait_result = result->wait(std::chrono::milliseconds(100));
  EXPECT_TRUE(wait_result.has_value());
}

TEST(SyncFenceTest, WaitTimesOut) {
  int const efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(efd, 0);

  // Don't signal — should time out
  auto result = drm::sync::SyncFence::import_fd(efd);
  ASSERT_TRUE(result.has_value());
  ::close(efd);

  auto wait_result = result->wait(std::chrono::milliseconds(1));
  EXPECT_FALSE(wait_result.has_value());
}
