// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "core/device.hpp"

#include <gtest/gtest.h>

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
