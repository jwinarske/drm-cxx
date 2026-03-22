// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "core/device.hpp"
#include "modeset/atomic.hpp"

class AtomicRequestTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto result = drm::Device::open("/dev/dri/card0");
    if (!result.has_value()) {
      GTEST_SKIP() << "No DRM device available";
    }
    dev_ = std::make_unique<drm::Device>(std::move(*result));

    auto cap = dev_->enable_atomic();
    if (!cap.has_value()) {
      GTEST_SKIP() << "Atomic modesetting not supported";
    }
  }

  std::unique_ptr<drm::Device> dev_;
};

TEST_F(AtomicRequestTest, ConstructAndDestroy) {
  drm::AtomicRequest req(*dev_);
  // Should not crash — verifies RAII allocation/deallocation
}

TEST_F(AtomicRequestTest, AddPropertyWithInvalidObject) {
  drm::AtomicRequest req(*dev_);
  // Adding a property should succeed at the API level (it just queues it)
  auto result = req.add_property(0, 0, 0);
  EXPECT_TRUE(result.has_value());
}

TEST_F(AtomicRequestTest, TestCommitWithEmptyRequest) {
  drm::AtomicRequest req(*dev_);
  // An empty atomic test-only commit should succeed
  auto result = req.test();
  EXPECT_TRUE(result.has_value());
}

TEST_F(AtomicRequestTest, MoveConstruct) {
  drm::AtomicRequest req1(*dev_);
  req1.add_property(0, 0, 0);

  drm::AtomicRequest req2(std::move(req1));
  // req2 should be usable
  auto result = req2.test();
  EXPECT_TRUE(result.has_value());
}

TEST_F(AtomicRequestTest, MoveAssign) {
  drm::AtomicRequest req1(*dev_);
  drm::AtomicRequest req2(*dev_);
  req1.add_property(0, 0, 0);

  req2 = std::move(req1);
  auto result = req2.test();
  EXPECT_TRUE(result.has_value());
}
