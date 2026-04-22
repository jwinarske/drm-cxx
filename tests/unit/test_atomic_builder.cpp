// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "core/device.hpp"
#include "modeset/atomic.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <utility>

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
  drm::AtomicRequest const req(*dev_);
  // Should not crash — verifies RAII allocation/deallocation
}

TEST_F(AtomicRequestTest, AddPropertyQueuesWithoutValidation) {
  drm::AtomicRequest req(*dev_);
  // libdrm's drmModeAtomicAddProperty rejects object_id == 0 or
  // property_id == 0 outright (EINVAL) but does NOT verify that the
  // ids refer to actual kernel objects — that check happens at
  // commit time. Any non-zero pair should queue successfully and
  // fail only when we try to commit it. Use sentinel values so the
  // intent is obvious.
  constexpr std::uint32_t k_sentinel_object = 0xDEADBEEFU;
  constexpr std::uint32_t k_sentinel_prop = 0xCAFEF00DU;
  const auto result = req.add_property(k_sentinel_object, k_sentinel_prop, 0U);
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
  // Moving an empty request is enough to exercise move semantics —
  // adding a property here would force the downstream req2.test()
  // to cope with an invalid-object commit, which isn't what this
  // test is about.
  drm::AtomicRequest req2(std::move(req1));
  auto result = req2.test();
  EXPECT_TRUE(result.has_value());
}

TEST_F(AtomicRequestTest, MoveAssign) {
  drm::AtomicRequest req1(*dev_);
  drm::AtomicRequest req2(*dev_);

  req2 = std::move(req1);
  auto result = req2.test();
  EXPECT_TRUE(result.has_value());
}
