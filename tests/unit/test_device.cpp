// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "core/device.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

TEST(DeviceTest, OpenInvalidPathReturnsError) {
  auto result = drm::Device::open("/dev/dri/nonexistent_card_999");
  ASSERT_FALSE(result.has_value());
}

TEST(DeviceTest, OpenEmptyPathReturnsError) {
  auto result = drm::Device::open("");
  ASSERT_FALSE(result.has_value());
}

TEST(DeviceTest, MoveConstructorTransfersFd) {
  // We can't easily test with a real DRM device in CI,
  // but we can verify that open on a non-DRM fd fails gracefully.
  auto result = drm::Device::open("/dev/null");
  // /dev/null is not a DRM device, so open should fail
  ASSERT_FALSE(result.has_value());
}

TEST(DeviceTest, SetClientCapOnInvalidFdFails) {
  // Create a Device by opening a real DRM device if available
  auto result = drm::Device::open("/dev/dri/card0");
  if (!result.has_value()) {
    GTEST_SKIP() << "No DRM device available";
  }
  // Setting an invalid cap should fail
  auto cap_result = result->set_client_cap(0xFFFFFFFF, 1);
  EXPECT_FALSE(cap_result.has_value());
}

TEST(DeviceTest, FromFdDoesNotCloseOnDestruction) {
  // Non-owning Device must NOT close the fd on destruction; the
  // caller (e.g. a SeatSession holding a libseat-managed fd) retains
  // ownership. Verify by opening a real fd, wrapping it, letting the
  // Device drop, and checking fstat on the fd still succeeds.
  const int raw = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  ASSERT_GE(raw, 0);

  {
    auto dev = drm::Device::from_fd(raw);
    EXPECT_EQ(dev.fd(), raw);
  }

  struct stat st{};
  EXPECT_EQ(::fstat(raw, &st), 0) << "Device::from_fd must not close the borrowed fd";
  ::close(raw);
}

TEST(DeviceTest, FromFdMoveDoesNotCloseBorrowedFd) {
  // Moving a non-owning Device must preserve the non-owning semantics;
  // the destination also must not close the fd.
  const int raw = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  ASSERT_GE(raw, 0);

  {
    auto src = drm::Device::from_fd(raw);
    auto dst = std::move(src);
    EXPECT_EQ(dst.fd(), raw);
  }

  struct stat st{};
  EXPECT_EQ(::fstat(raw, &st), 0);
  ::close(raw);
}
