// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// drm::dumb::Buffer against a live card (vkms, or DRM_CXX_TEST_CARD).

#include "core/device.hpp"
#include "dumb/buffer.hpp"
#include "vkms_node.hpp"

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>

#include <gtest/gtest.h>
#include <optional>
#include <utility>

namespace {

drm::dumb::Config argb_config() {
  drm::dumb::Config cfg;
  cfg.width = 64;
  cfg.height = 64;
  cfg.drm_format = DRM_FORMAT_ARGB8888;
  cfg.bpp = 32;
  cfg.add_fb = false;
  return cfg;
}

}  // namespace

// After session loss the next open of the card takes the closed descriptor's
// number, and GEM handles are numbered per file. Destroying a buffer of the old
// device must not destroy the new device's buffer of the same handle (#370).
TEST(DumbBufferVkms, DestroyAfterDeviceClosedSparesTheNextOpen) {
  const auto node = drm::test::find_test_card_or_vkms();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  std::optional<drm::Device> before;
  if (auto d = drm::Device::open(*node); d) {
    before.emplace(std::move(*d));
  } else {
    GTEST_SKIP() << "cannot open " << *node;
  }
  auto old_r = drm::dumb::Buffer::create(*before, argb_config());
  ASSERT_TRUE(old_r.has_value()) << old_r.error().message();
  std::optional<drm::dumb::Buffer> old(std::move(*old_r));
  const int old_fd = before->fd();
  before.reset();  // session loss: the device's descriptor closes

  auto after = drm::Device::open(*node);
  ASSERT_TRUE(after.has_value()) << after.error().message();
  auto fresh = drm::dumb::Buffer::create(*after, argb_config());
  ASSERT_TRUE(fresh.has_value()) << fresh.error().message();
  ASSERT_EQ(after->fd(), old_fd) << "the reopen should take the closed number";

  old.reset();

  drm_mode_map_dumb map{};
  map.handle = fresh->handle();
  EXPECT_EQ(drmIoctl(after->fd(), DRM_IOCTL_MODE_MAP_DUMB, &map), 0)
      << "the new device's buffer was destroyed through the old one's number";
}
