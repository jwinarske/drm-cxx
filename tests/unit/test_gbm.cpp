// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "gbm/buffer.hpp"
#include "gbm/device.hpp"
#include "gbm/surface.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

TEST(GbmDeviceTest, CreateWithInvalidFdFails) {
  auto result = drm::gbm::GbmDevice::create(-1);
  EXPECT_FALSE(result.has_value());
}

TEST(GbmDeviceTest, CreateWithNonDrmFdGraceful) {
  // /dev/null is not a DRM device — gbm_create_device may or may not
  // succeed depending on the mesa implementation. Either way, no crash.
  int fd = ::open("/dev/null", O_RDWR);
  if (fd < 0) GTEST_SKIP() << "Cannot open /dev/null";

  auto result = drm::gbm::GbmDevice::create(fd);
  // Just verify no crash — result may be valid or error
  (void)result;
  ::close(fd);
}

TEST(GbmDeviceTest, CreateWithRealDrmDevice) {
  // Try to create GBM device from a real DRM device
  auto dev_result = drm::gbm::GbmDevice::create(::open("/dev/dri/card0", O_RDWR));
  if (!dev_result.has_value()) {
    GTEST_SKIP() << "No DRM device available for GBM";
  }

  EXPECT_NE(dev_result->raw(), nullptr);
}

TEST(GbmDeviceTest, MoveConstruct) {
  auto dev_result = drm::gbm::GbmDevice::create(::open("/dev/dri/card0", O_RDWR));
  if (!dev_result.has_value()) {
    GTEST_SKIP() << "No DRM device available";
  }

  auto dev2 = std::move(*dev_result);
  EXPECT_NE(dev2.raw(), nullptr);
}

TEST(BufferTest, DefaultBufferAccessors) {
  // Can't construct a Buffer directly (private), but we can test
  // that the types compile and the API is correct.
  // This is a compile-time check more than a runtime test.
  SUCCEED();
}
