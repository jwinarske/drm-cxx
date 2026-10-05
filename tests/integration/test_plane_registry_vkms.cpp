// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// PlaneRegistry::enumerate against a real device: the capability fields it
// fills from the kernel. Runs on vkms (`modprobe vkms enable_cursor=1`), or on
// DRM_CXX_TEST_CARD.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm.h>
#include <xf86drm.h>

#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

std::optional<std::string> find_card() {
  if (const char* env = std::getenv("DRM_CXX_TEST_CARD"); env != nullptr && *env != '\0') {
    return std::string(env);
  }
  for (int idx = 0; idx < 8; ++idx) {
    std::string path = "/dev/dri/card" + std::to_string(idx);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr ver = drmGetVersion(fd);
    const bool is_vkms = (ver != nullptr) && (ver->name != nullptr) &&
                         std::string(ver->name, ver->name_len) == "vkms";
    if (ver != nullptr) {
      drmFreeVersion(ver);
    }
    ::close(fd);
    if (is_vkms) {
      return path;
    }
  }
  return std::nullopt;
}

}  // namespace

// Cursor planes carry the device's DRM_CAP_CURSOR_WIDTH/HEIGHT; no other plane
// type has a cursor limit.
TEST(PlaneRegistryVkms, CursorPlanesCarryDeviceCursorCaps) {
  const auto path = find_card();
  if (!path.has_value()) {
    GTEST_SKIP() << "vkms not loaded; modprobe vkms enable_cursor=1.";
  }
  auto dev = drm::Device::open(*path);
  ASSERT_TRUE(dev.has_value()) << dev.error().message();
  ASSERT_TRUE(dev->enable_universal_planes().has_value());

  std::uint64_t cap_w = 0;
  std::uint64_t cap_h = 0;
  const bool have_caps = drmGetCap(dev->fd(), DRM_CAP_CURSOR_WIDTH, &cap_w) == 0 &&
                         drmGetCap(dev->fd(), DRM_CAP_CURSOR_HEIGHT, &cap_h) == 0;

  auto reg = drm::planes::PlaneRegistry::enumerate(*dev);
  ASSERT_TRUE(reg.has_value()) << reg.error().message();
  std::size_t cursors = 0;
  for (const auto& p : reg->all()) {
    if (p.type == drm::planes::DRMPlaneType::CURSOR) {
      ++cursors;
      EXPECT_EQ(p.cursor_max_w, have_caps ? cap_w : 0U) << "plane " << p.id;
      EXPECT_EQ(p.cursor_max_h, have_caps ? cap_h : 0U) << "plane " << p.id;
    } else {
      EXPECT_EQ(p.cursor_max_w, 0U) << "plane " << p.id;
      EXPECT_EQ(p.cursor_max_h, 0U) << "plane " << p.id;
    }
  }
  if (cursors == 0) {
    GTEST_SKIP() << "no cursor plane on this device (vkms: enable_cursor=1)";
  }
}
