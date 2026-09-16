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

// This table stands in for drmModeGetConnectorTypeName, which can return
// nullptr on older libdrm, so it has to answer with the spelling libdrm and the
// kernel use rather than a prettier one. SVIDEO is pinned because it is the
// entry that drifted -- it read "S-Video" while libdrm returns "SVIDEO". Every
// other entry in the table was checked against libdrm and already matched.
TEST(ModeListTest, ConnectorTypeNameMatchesLibdrmSpelling) {
  EXPECT_STREQ(drm::display::connector_type_name(DRM_MODE_CONNECTOR_SVIDEO), "SVIDEO");
  EXPECT_STREQ(drm::display::connector_type_name(DRM_MODE_CONNECTOR_Composite), "Composite");
  EXPECT_STREQ(drm::display::connector_type_name(DRM_MODE_CONNECTOR_LVDS), "LVDS");
  EXPECT_STREQ(drm::display::connector_type_name(DRM_MODE_CONNECTOR_VGA), "VGA");
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
