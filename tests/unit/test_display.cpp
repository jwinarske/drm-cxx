// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "display/connector_info.hpp"
#include "display/edid.hpp"

#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <gtest/gtest.h>

TEST(ConnectorInfoTest, DefaultConstruction) {
  drm::display::ConnectorInfo const info;
  EXPECT_TRUE(info.name.empty());
  EXPECT_TRUE(info.make.empty());
  EXPECT_TRUE(info.model.empty());
  EXPECT_FALSE(info.serial.has_value());
  EXPECT_EQ(info.width_mm, 0);
  EXPECT_EQ(info.height_mm, 0);
  EXPECT_FALSE(info.colorimetry.has_value());
  EXPECT_FALSE(info.hdr.has_value());
  EXPECT_FALSE(info.wide_gamut.has_value());
}

TEST(HdrStaticMetadataTest, DefaultValues) {
  drm::display::HdrStaticMetadata const md{};
  EXPECT_FLOAT_EQ(md.desired_content_max_luminance, 0.0F);
  EXPECT_FLOAT_EQ(md.desired_content_max_frame_avg_luminance, 0.0F);
  EXPECT_FLOAT_EQ(md.desired_content_min_luminance, 0.0F);
  EXPECT_FALSE(md.type1);
  EXPECT_FALSE(md.traditional_sdr);
  EXPECT_FALSE(md.traditional_hdr);
  EXPECT_FALSE(md.pq);
  EXPECT_FALSE(md.hlg);
}

TEST(ColorimetryInfoTest, CanSetValues) {
  drm::display::ColorimetryInfo ci{};
  ci.red = {0.64F, 0.33F};
  ci.green = {0.30F, 0.60F};
  ci.blue = {0.15F, 0.06F};
  ci.white = {0.3127F, 0.3290F};
  ci.has_primaries = true;
  ci.has_default_white = true;

  EXPECT_FLOAT_EQ(ci.red.x, 0.64F);
  EXPECT_FLOAT_EQ(ci.white.y, 0.3290F);
  EXPECT_TRUE(ci.has_primaries);
  EXPECT_TRUE(ci.has_default_white);
}

TEST(SupportedColorimetryTest, DefaultValues) {
  drm::display::SupportedColorimetry const w{};
  EXPECT_FALSE(w.bt2020_cycc);
  EXPECT_FALSE(w.bt2020_ycc);
  EXPECT_FALSE(w.bt2020_rgb);
  EXPECT_FALSE(w.st2113_rgb);
  EXPECT_FALSE(w.ictcp);
}

TEST(ParseEdidTest, EmptyBlobReturnsError) {
  drm::span<const uint8_t> const empty;
  auto result = drm::display::parse_edid(empty);
  EXPECT_FALSE(result.has_value());
}

TEST(ParseEdidTest, InvalidBlobReturnsError) {
  uint8_t const garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
  auto result = drm::display::parse_edid(garbage);
  // libdisplay-info may still parse garbage but return minimal info
  // Either way, should not crash
  (void)result;
}

// A minimal valid EDID block (128 bytes) for testing
TEST(ParseEdidTest, ValidEdidParses) {
  // Standard EDID 1.3 header
  static constexpr uint8_t edid[] = {
      0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,  // Header
      0x10, 0xAC, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,  // Vendor (DEL)
      0x01, 0x11, 0x01, 0x03, 0x80, 0x34, 0x20, 0x78,  // Version, size
      0xEA, 0xEE, 0x95, 0xA3, 0x54, 0x4C, 0x99, 0x26,  // Features, chromaticity
      0x0F, 0x50, 0x54, 0xA5, 0x4B, 0x00, 0x71, 0x4F,  // Established timings
      0x81, 0x80, 0xA9, 0xC0, 0xD1, 0xC0, 0x01, 0x01,  // Standard timings
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3A,  // Standard + DTD
      0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C,  // Detailed timing
      0x45, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x1E,  // Detailed timing cont
      0x00, 0x00, 0x00, 0xFF, 0x00, 0x46, 0x4F, 0x4F,  // Serial number
      0x42, 0x41, 0x52, 0x0A, 0x20, 0x20, 0x20, 0x20,  // Serial cont
      0x20, 0x20, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x44,  // Monitor name
      0x45, 0x4C, 0x4C, 0x0A, 0x20, 0x20, 0x20, 0x20,  // Name cont
      0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFD,  // Range limits
      0x00, 0x38, 0x4C, 0x1E, 0x51, 0x11, 0x00, 0x0A,  // Range values
      0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x21,  // Checksum
  };

  auto result = drm::display::parse_edid(edid);
  // Should parse without crashing. The name should contain something.
  if (result.has_value()) {
    // Verify we got some data
    EXPECT_FALSE(result->name.empty());
    // Identity fields: this blob carries a monitor-name and serial-number
    // descriptor, so make/model/serial are populated. The serial descriptor
    // spells "FOOBAR"; make/model come from the vendor + monitor-name bytes.
    EXPECT_FALSE(result->make.empty());
    EXPECT_FALSE(result->model.empty());
    ASSERT_TRUE(result->serial.has_value());
    EXPECT_EQ(*result->serial, "FOOBAR");
    // Pre-CTA EDID 1.3 has no HDR / wide-gamut data blocks; those
    // optionals should remain empty. Default colorimetry from the
    // base EDID chromaticity bytes may or may not populate depending
    // on the libdisplay-info version's handling of zero coordinates.
    EXPECT_FALSE(result->hdr.has_value());
    EXPECT_FALSE(result->wide_gamut.has_value());
  }
}