// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "display/mode_list.hpp"

#include <drm_mode.h>

#include <gtest/gtest.h>

namespace {

TEST(ModeListTest, ConnectorTypeName) {
  EXPECT_STREQ(drm::display::connector_type_name(DRM_MODE_CONNECTOR_HDMIA), "HDMI-A");
  EXPECT_STREQ(drm::display::connector_type_name(DRM_MODE_CONNECTOR_eDP), "eDP");
  EXPECT_STREQ(drm::display::connector_type_name(DRM_MODE_CONNECTOR_DisplayPort), "DP");
  EXPECT_STREQ(drm::display::connector_type_name(DRM_MODE_CONNECTOR_DSI), "DSI");
  EXPECT_STREQ(drm::display::connector_type_name(DRM_MODE_CONNECTOR_VIRTUAL), "Virtual");
  // Unknown / out-of-range never returns nullptr.
  EXPECT_STREQ(drm::display::connector_type_name(0xFFFFU), "Unknown");
}

TEST(ModeListTest, ConnectorName) {
  drm::display::ConnectorModes c;
  c.connector_type = DRM_MODE_CONNECTOR_HDMIA;
  c.connector_type_id = 1;
  EXPECT_EQ(c.name(), "HDMI-A-1");

  c.connector_type = DRM_MODE_CONNECTOR_eDP;
  c.connector_type_id = 2;
  EXPECT_EQ(c.name(), "eDP-2");
}

}  // namespace
