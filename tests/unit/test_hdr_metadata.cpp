// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "display/hdr_metadata.hpp"

#include <drm/drm_mode.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <utility>

namespace {

using drm::display::HdrSourceMetadata;
using drm::display::TransferFunction;

/// Construct a typical HDR10 mastering description: BT.2020 primaries
/// (CIE xy from BT.2020 spec), D65 white, 1000-nit display, 1000 MaxCLL,
/// 400 MaxFALL.
HdrSourceMetadata make_bt2020_pq_sample() {
  HdrSourceMetadata src;
  src.eotf = TransferFunction::SmpteSt2084Pq;
  src.display_primaries.red = {0.708F, 0.292F};
  src.display_primaries.green = {0.170F, 0.797F};
  src.display_primaries.blue = {0.131F, 0.046F};
  src.display_primaries.white = {0.3127F, 0.3290F};
  src.display_primaries.has_primaries = true;
  src.display_primaries.has_default_white = true;
  src.max_display_mastering_luminance = 1000;
  src.min_display_mastering_luminance = 50;  // 0.005 cd/m² (50 * 0.0001)
  src.max_content_light_level = 1000;
  src.max_frame_average_light_level = 400;
  return src;
}

TEST(HdrMetadataTest, SerializeProducesKernelStructLayout) {
  const auto src = make_bt2020_pq_sample();
  const auto bytes = drm::display::serialize_hdr_metadata(src);
  ASSERT_EQ(bytes.size(), sizeof(hdr_output_metadata));

  hdr_output_metadata out{};
  std::memcpy(&out, bytes.data(), bytes.size());

  EXPECT_EQ(out.metadata_type, 0U);             // HDMI_STATIC_METADATA_TYPE1
  EXPECT_EQ(out.hdmi_metadata_type1.eotf, 2U);  // SMPTE_ST2084
  EXPECT_EQ(out.hdmi_metadata_type1.metadata_type, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.max_display_mastering_luminance, 1000U);
  EXPECT_EQ(out.hdmi_metadata_type1.min_display_mastering_luminance, 50U);
  EXPECT_EQ(out.hdmi_metadata_type1.max_cll, 1000U);
  EXPECT_EQ(out.hdmi_metadata_type1.max_fall, 400U);
}

TEST(HdrMetadataTest, PrimariesAreReorderedToGreenBlueRed) {
  // CTA-861.3 §6.9.1.5 says primaries[0]=green, [1]=blue, [2]=red.
  // ColorimetryInfo is named-by-color; verify the serializer maps
  // green → primaries[0], blue → primaries[1], red → primaries[2].
  const auto src = make_bt2020_pq_sample();
  const auto bytes = drm::display::serialize_hdr_metadata(src);
  hdr_output_metadata out{};
  std::memcpy(&out, bytes.data(), bytes.size());

  // Primaries encoded as 0.00002 steps (0xC350 == 1.0). Green xy
  // 0.170/0.797 → 8500/39850; Blue 0.131/0.046 → 6550/2300; Red
  // 0.708/0.292 → 35400/14600. Allow ±1 LSB rounding.
  EXPECT_NEAR(out.hdmi_metadata_type1.display_primaries[0].x, 8500U, 1);
  EXPECT_NEAR(out.hdmi_metadata_type1.display_primaries[0].y, 39850U, 1);
  EXPECT_NEAR(out.hdmi_metadata_type1.display_primaries[1].x, 6550U, 1);
  EXPECT_NEAR(out.hdmi_metadata_type1.display_primaries[1].y, 2300U, 1);
  EXPECT_NEAR(out.hdmi_metadata_type1.display_primaries[2].x, 35400U, 1);
  EXPECT_NEAR(out.hdmi_metadata_type1.display_primaries[2].y, 14600U, 1);

  // White point D65: 0.3127 / 0.3290 → 15635 / 16450.
  EXPECT_NEAR(out.hdmi_metadata_type1.white_point.x, 15635U, 1);
  EXPECT_NEAR(out.hdmi_metadata_type1.white_point.y, 16450U, 1);
}

TEST(HdrMetadataTest, OutOfRangeFloatsClampToValidU16) {
  HdrSourceMetadata src;
  src.display_primaries.red = {-0.1F, 5.0F};    // negative + greater than 1.0
  src.display_primaries.green = {0.0F, 1.31F};  // 1.31 * 50000 = 65500 (in range)
  src.display_primaries.blue = {2.0F, 0.0F};    // 2.0 * 50000 = 100000 (clamps to 0xFFFF)
  src.display_primaries.white = {0.0F, 0.0F};

  const auto bytes = drm::display::serialize_hdr_metadata(src);
  hdr_output_metadata out{};
  std::memcpy(&out, bytes.data(), bytes.size());

  // Red: -0.1 → 0 (negative clamp), 5.0 → 0xFFFF (overflow clamp).
  // Display primaries[2] is red after the GBR reorder.
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[2].x, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[2].y, 0xFFFFU);
  // Green primaries[0]: 0.0 → 0, 1.31 → 65500.
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[0].x, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[0].y, 65500U);
  // Blue primaries[1]: 2.0 → 0xFFFF.
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[1].x, 0xFFFFU);
}

TEST(HdrMetadataTest, EotfEnumIntegersMatchHdmiSpec) {
  constexpr std::array<std::pair<TransferFunction, std::uint8_t>, 4> k_cases{{
      {TransferFunction::TraditionalGammaSdr, 0},
      {TransferFunction::TraditionalGammaHdr, 1},
      {TransferFunction::SmpteSt2084Pq, 2},
      {TransferFunction::Bt2100Hlg, 3},
  }};
  HdrSourceMetadata src;
  for (const auto& tc : k_cases) {
    src.eotf = tc.first;
    const auto bytes = drm::display::serialize_hdr_metadata(src);
    hdr_output_metadata out{};
    std::memcpy(&out, bytes.data(), bytes.size());
    EXPECT_EQ(out.hdmi_metadata_type1.eotf, tc.second);
  }
}

TEST(HdrMetadataTest, HashIsStableForIdenticalContent) {
  const auto a = make_bt2020_pq_sample();
  const auto b = make_bt2020_pq_sample();
  EXPECT_EQ(drm::display::hdr_metadata_hash(a), drm::display::hdr_metadata_hash(b));
}

TEST(HdrMetadataTest, HashChangesWhenAnyFieldChanges) {
  const auto base = make_bt2020_pq_sample();
  const auto base_hash = drm::display::hdr_metadata_hash(base);

  // EOTF.
  HdrSourceMetadata m = base;
  m.eotf = TransferFunction::Bt2100Hlg;
  EXPECT_NE(drm::display::hdr_metadata_hash(m), base_hash) << "eotf";

  // MaxCLL.
  m = base;
  m.max_content_light_level = 999;
  EXPECT_NE(drm::display::hdr_metadata_hash(m), base_hash) << "MaxCLL";

  // MaxFALL.
  m = base;
  m.max_frame_average_light_level = 399;
  EXPECT_NE(drm::display::hdr_metadata_hash(m), base_hash) << "MaxFALL";

  // Mastering display max luminance.
  m = base;
  m.max_display_mastering_luminance = 4000;
  EXPECT_NE(drm::display::hdr_metadata_hash(m), base_hash) << "max_display_mastering";

  // Mastering display min luminance.
  m = base;
  m.min_display_mastering_luminance = 5;
  EXPECT_NE(drm::display::hdr_metadata_hash(m), base_hash) << "min_display_mastering";

  // Single primary coordinate.
  m = base;
  m.display_primaries.red.x = 0.700F;
  EXPECT_NE(drm::display::hdr_metadata_hash(m), base_hash) << "red.x";

  // White point.
  m = base;
  m.display_primaries.white.x = 0.300F;
  EXPECT_NE(drm::display::hdr_metadata_hash(m), base_hash) << "white.x";
}

TEST(HdrMetadataTest, DefaultMetadataIsAllZeroExceptStructure) {
  // A default-constructed HdrSourceMetadata serializes to an all-zero
  // hdr_output_metadata with eotf=0 (TraditionalGammaSdr) — the
  // "clear HDR signaling" shape callers may emit via blob_id=0
  // instead, but the struct itself should still be valid.
  const HdrSourceMetadata src;
  const auto bytes = drm::display::serialize_hdr_metadata(src);
  hdr_output_metadata out{};
  std::memcpy(&out, bytes.data(), bytes.size());

  EXPECT_EQ(out.metadata_type, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.eotf, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.max_cll, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.max_fall, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[0].x, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[0].y, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[1].x, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[1].y, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[2].x, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.display_primaries[2].y, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.white_point.x, 0U);
  EXPECT_EQ(out.hdmi_metadata_type1.white_point.y, 0U);
}

TEST(HdrMetadataBlobTest, DefaultConstructedIsEmpty) {
  const drm::display::HdrMetadataBlob blob;
  EXPECT_EQ(blob.blob_id(), 0U);
  EXPECT_EQ(blob.content_hash(), 0U);
  EXPECT_FALSE(static_cast<bool>(blob));
}

// Live blob create / readback / destroy is covered by the
// vkms integration test once it lands; reaching `create()` here would
// require either a real Device or a libdrm mock, neither of which fits
// a unit test.

}  // namespace
