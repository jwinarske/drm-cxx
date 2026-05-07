// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "display/connector_capabilities.hpp"

#include <gtest/gtest.h>

namespace {

using drm::display::BroadcastRgb;
using drm::display::Colorspace;
using drm::display::ConnectorCapabilities;

TEST(ConnectorCapabilitiesTest, DefaultConstructionAllAbsent) {
  ConnectorCapabilities const caps;
  EXPECT_EQ(caps.connector_id, 0U);
  EXPECT_FALSE(caps.has_colorspace);
  EXPECT_FALSE(caps.has_max_bpc);
  EXPECT_FALSE(caps.has_hdr_output_metadata);
  EXPECT_FALSE(caps.has_broadcast_rgb);
  EXPECT_FALSE(caps.colorspace_default.has_value());
  EXPECT_FALSE(caps.colorspace_bt2020_rgb.has_value());
  EXPECT_FALSE(caps.max_bpc_min.has_value());
  EXPECT_FALSE(caps.max_bpc_max.has_value());
  EXPECT_FALSE(caps.broadcast_rgb_full.has_value());
}

TEST(ConnectorCapabilitiesTest, CanSignalHdrRequiresBlobAndMaxBpcGe10) {
  ConnectorCapabilities caps;
  EXPECT_FALSE(caps.can_signal_hdr());

  // HDR_OUTPUT_METADATA without max_bpc — not enough.
  caps.has_hdr_output_metadata = true;
  EXPECT_FALSE(caps.can_signal_hdr());

  // max_bpc present but capped at 8 — not enough.
  caps.has_max_bpc = true;
  caps.max_bpc_min = 6;
  caps.max_bpc_max = 8;
  EXPECT_FALSE(caps.can_signal_hdr());

  // 10-bit ceiling — yes.
  caps.max_bpc_max = 10;
  EXPECT_TRUE(caps.can_signal_hdr());

  // 12-bit ceiling — also yes.
  caps.max_bpc_max = 12;
  EXPECT_TRUE(caps.can_signal_hdr());

  // Drop the blob — back to no.
  caps.has_hdr_output_metadata = false;
  EXPECT_FALSE(caps.can_signal_hdr());
}

TEST(ConnectorCapabilitiesTest, ColorspaceLookupRoundTrip) {
  // Synthesise a typical amdgpu connector — Default + the four BT
  // entries that real RDNA hardware advertises, plus opRGB. Other
  // entries stay nullopt.
  ConnectorCapabilities caps;
  caps.has_colorspace = true;
  caps.colorspace_default = 0;
  caps.colorspace_bt709_ycc = 6;
  caps.colorspace_bt2020_rgb = 9;
  caps.colorspace_bt2020_ycc = 11;
  caps.colorspace_oprgb = 4;

  EXPECT_EQ(caps.colorspace_value(Colorspace::Default).value_or(99), 0U);
  EXPECT_EQ(caps.colorspace_value(Colorspace::Bt709Ycc).value_or(99), 6U);
  EXPECT_EQ(caps.colorspace_value(Colorspace::Bt2020Rgb).value_or(99), 9U);
  EXPECT_EQ(caps.colorspace_value(Colorspace::Bt2020Ycc).value_or(99), 11U);
  EXPECT_EQ(caps.colorspace_value(Colorspace::OpRgb).value_or(99), 4U);

  // Entries the synthetic connector doesn't advertise should be
  // nullopt — callers must check before writing.
  EXPECT_FALSE(caps.colorspace_value(Colorspace::DciP3RgbD65).has_value());
  EXPECT_FALSE(caps.colorspace_value(Colorspace::Bt2020Cycc).has_value());
  EXPECT_FALSE(caps.colorspace_value(Colorspace::SmpteRf170mYcc).has_value());
}

TEST(ConnectorCapabilitiesTest, BroadcastRgbLookupRoundTrip) {
  ConnectorCapabilities caps;
  caps.has_broadcast_rgb = true;
  caps.broadcast_rgb_automatic = 0;
  caps.broadcast_rgb_full = 1;
  caps.broadcast_rgb_limited = 2;

  EXPECT_EQ(caps.broadcast_rgb_value(BroadcastRgb::Automatic).value_or(99), 0U);
  EXPECT_EQ(caps.broadcast_rgb_value(BroadcastRgb::Full).value_or(99), 1U);
  EXPECT_EQ(caps.broadcast_rgb_value(BroadcastRgb::Limited).value_or(99), 2U);
}

TEST(ConnectorCapabilitiesTest, BroadcastRgbAbsentReturnsNullopt) {
  ConnectorCapabilities const caps;
  EXPECT_FALSE(caps.broadcast_rgb_value(BroadcastRgb::Automatic).has_value());
  EXPECT_FALSE(caps.broadcast_rgb_value(BroadcastRgb::Full).has_value());
  EXPECT_FALSE(caps.broadcast_rgb_value(BroadcastRgb::Limited).has_value());
}

TEST(ConnectorCapabilitiesTest, HdrCapableConnectorRecognised) {
  // Realistic HDR10 connector: HDR_OUTPUT_METADATA + max_bpc up to 12,
  // BT.2020 colorspace entries advertised.
  ConnectorCapabilities caps;
  caps.connector_id = 42;
  caps.has_hdr_output_metadata = true;
  caps.has_max_bpc = true;
  caps.max_bpc_min = 6;
  caps.max_bpc_max = 12;
  caps.max_bpc_current = 8;
  caps.has_colorspace = true;
  caps.colorspace_default = 0;
  caps.colorspace_bt2020_rgb = 9;
  caps.colorspace_bt2020_ycc = 11;

  EXPECT_TRUE(caps.can_signal_hdr());
  EXPECT_TRUE(caps.colorspace_value(Colorspace::Bt2020Rgb).has_value());
  EXPECT_TRUE(caps.colorspace_value(Colorspace::Bt2020Ycc).has_value());
}

}  // namespace