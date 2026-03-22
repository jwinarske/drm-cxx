// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "planes/plane_registry.hpp"

#include <gtest/gtest.h>

TEST(PlaneCapabilitiesTest, SupportsFormat) {
  drm::planes::PlaneCapabilities caps;
  caps.formats = {0x34325258, 0x34325241};  // XRGB8888, ARGB8888

  EXPECT_TRUE(caps.supports_format(0x34325258));
  EXPECT_TRUE(caps.supports_format(0x34325241));
  EXPECT_FALSE(caps.supports_format(0x36314752));  // RGB565
}

TEST(PlaneCapabilitiesTest, CrtcCompatibility) {
  drm::planes::PlaneCapabilities caps;
  caps.possible_crtcs = 0b0101;  // CRTCs 0 and 2

  EXPECT_TRUE(caps.compatible_with_crtc(0));
  EXPECT_FALSE(caps.compatible_with_crtc(1));
  EXPECT_TRUE(caps.compatible_with_crtc(2));
  EXPECT_FALSE(caps.compatible_with_crtc(3));
}

TEST(PlaneCapabilitiesTest, DefaultValues) {
  drm::planes::PlaneCapabilities caps;

  EXPECT_EQ(caps.id, 0u);
  EXPECT_EQ(caps.possible_crtcs, 0u);
  EXPECT_EQ(caps.type, drm::planes::DRMPlaneType::OVERLAY);
  EXPECT_TRUE(caps.formats.empty());
  EXPECT_FALSE(caps.zpos_min.has_value());
  EXPECT_FALSE(caps.zpos_max.has_value());
  EXPECT_FALSE(caps.supports_rotation);
  EXPECT_FALSE(caps.supports_scaling);
}
