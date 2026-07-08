// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "planes/allocator.hpp"
#include "planes/plane_registry.hpp"

#include <drm_fourcc.h>

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>

TEST(PlaneCapabilitiesTest, SupportsFormat) {
  drm::planes::PlaneCapabilities caps;
  caps.formats = {0x34325258, 0x34325241};  // XRGB8888, ARGB8888

  EXPECT_TRUE(caps.supports_format(0x34325258));
  EXPECT_TRUE(caps.supports_format(0x34325241));
  EXPECT_FALSE(caps.supports_format(0x36314752));  // RGB565
}

TEST(PlaneCapabilitiesTest, LayerFitsPlaneFromInFormats) {
  // IN_FORMATS present: the (format, modifier) pairs are authoritative. Exact
  // pairs match; INVALID is LINEAR-equivalent; a vendor tiling matches only where
  // it is listed; a format absent from the table is rejected.
  constexpr uint64_t k_afbc_like = (1ULL << 56) | 1;
  drm::planes::PlaneCapabilities caps;
  caps.formats = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_RGB565};
  caps.format_table = drm::fmt::FormatTable::from_pairs(std::vector<std::pair<uint32_t, uint64_t>>{
      {DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR},
      {DRM_FORMAT_XRGB8888, k_afbc_like},
      {DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR},
  });
  caps.has_format_modifiers = true;
  caps.build_format_metadata();

  const auto fits = [&](uint32_t f, uint64_t m) {
    return drm::fmt::layer_fits_plane(caps.format_table, f, drm::fmt::Modifier{m});
  };
  EXPECT_TRUE(fits(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR));
  EXPECT_TRUE(fits(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID));  // INVALID == LINEAR
  EXPECT_TRUE(fits(DRM_FORMAT_XRGB8888, k_afbc_like));             // vendor tiling listed
  EXPECT_TRUE(fits(DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR));
  EXPECT_FALSE(fits(DRM_FORMAT_ARGB8888, k_afbc_like));          // tiling not listed for ARGB
  EXPECT_FALSE(fits(DRM_FORMAT_RGB565, DRM_FORMAT_MOD_LINEAR));  // format not in the table
}

TEST(PlaneCapabilitiesTest, LayerFitsPlaneLinearOnlyWhenNoInFormats) {
  // No IN_FORMATS: build_format_metadata synthesizes LINEAR for each bare format.
  // LINEAR / INVALID fit; a non-trivial modifier does not; an absent format does not.
  drm::planes::PlaneCapabilities caps;
  caps.formats = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
  caps.has_format_modifiers = false;
  caps.build_format_metadata();

  const auto fits = [&](uint32_t f, uint64_t m) {
    return drm::fmt::layer_fits_plane(caps.format_table, f, drm::fmt::Modifier{m});
  };
  EXPECT_TRUE(fits(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR));
  EXPECT_TRUE(fits(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID));
  EXPECT_TRUE(fits(DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR));
  constexpr uint64_t k_afbc_like = (1ULL << 56) | 1;
  EXPECT_FALSE(fits(DRM_FORMAT_XRGB8888, k_afbc_like));          // no IN_FORMATS evidence
  EXPECT_FALSE(fits(DRM_FORMAT_RGB565, DRM_FORMAT_MOD_LINEAR));  // format not advertised
}

TEST(PlaneCapabilitiesTest, FormatTableLinearOnlyWhenNoInFormats) {
  // No IN_FORMATS: build_format_metadata records LINEAR for each bare format.
  drm::planes::PlaneCapabilities caps;
  caps.formats = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
  caps.has_format_modifiers = false;
  caps.build_format_metadata();

  EXPECT_TRUE(
      caps.format_table.supports(DRM_FORMAT_XRGB8888, drm::fmt::Modifier{DRM_FORMAT_MOD_LINEAR}));
  EXPECT_TRUE(
      caps.format_table.supports(DRM_FORMAT_ARGB8888, drm::fmt::Modifier{DRM_FORMAT_MOD_LINEAR}));
  constexpr uint64_t k_afbc_like = (1ULL << 56) | 1;
  EXPECT_FALSE(caps.format_table.supports(DRM_FORMAT_XRGB8888, drm::fmt::Modifier{k_afbc_like}));
  EXPECT_EQ(caps.bandwidth_class(DRM_FORMAT_MOD_LINEAR), drm::fmt::BandwidthClass::Linear);
}

TEST(PlaneCapabilitiesTest, BandwidthClassMatchesClassify) {
  // Precomputed per-modifier class equals a live classify() for each advertised
  // modifier, and an unadvertised modifier falls back to classify() (not Linear).
  constexpr uint64_t k_afbc_like = (1ULL << 56) | 1;
  drm::planes::PlaneCapabilities caps;
  caps.format_table = drm::fmt::FormatTable::from_pairs(std::vector<std::pair<uint32_t, uint64_t>>{
      {DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR},
      {DRM_FORMAT_XRGB8888, k_afbc_like},
  });
  caps.has_format_modifiers = true;
  caps.build_format_metadata();

  EXPECT_EQ(caps.bandwidth_class(DRM_FORMAT_MOD_LINEAR),
            drm::fmt::classify(drm::fmt::Modifier{DRM_FORMAT_MOD_LINEAR}));
  EXPECT_EQ(caps.bandwidth_class(k_afbc_like), drm::fmt::classify(drm::fmt::Modifier{k_afbc_like}));
  constexpr uint64_t k_unadvertised = (uint64_t{0x02} << 56) | 9;
  EXPECT_EQ(caps.bandwidth_class(k_unadvertised),
            drm::fmt::classify(drm::fmt::Modifier{k_unadvertised}));
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
  drm::planes::PlaneCapabilities const caps;

  EXPECT_EQ(caps.id, 0U);
  EXPECT_EQ(caps.possible_crtcs, 0U);
  EXPECT_EQ(caps.type, drm::planes::DRMPlaneType::OVERLAY);
  EXPECT_TRUE(caps.formats.empty());
  EXPECT_FALSE(caps.zpos_min.has_value());
  EXPECT_FALSE(caps.zpos_max.has_value());
  EXPECT_FALSE(caps.supports_rotation);
  EXPECT_FALSE(caps.supports_scaling);
}

TEST(PowerAwareBias, BandwidthClassBonus) {
  using drm::planes::bandwidth_class_bonus;
  // LINEAR composites cheaply — no placement bonus.
  EXPECT_EQ(bandwidth_class_bonus(DRM_FORMAT_MOD_LINEAR), 0);
  // Tiled: better DRAM locality — small bonus.
  EXPECT_EQ(bandwidth_class_bonus(DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED), 1);
  // Compressed (AFBC / DCC): biggest saving — direct scanout avoids a GPU
  // decompress, so the matcher should most strongly keep it on a plane.
  EXPECT_EQ(bandwidth_class_bonus(DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16)), 2);
  EXPECT_EQ(bandwidth_class_bonus(DRM_FORMAT_MOD_QCOM_COMPRESSED), 2);
  // The ordering (compressed > tiled > linear) is what biases placement.
  EXPECT_GT(bandwidth_class_bonus(DRM_FORMAT_MOD_QCOM_COMPRESSED),
            bandwidth_class_bonus(DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED));
  EXPECT_GT(bandwidth_class_bonus(DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED),
            bandwidth_class_bonus(DRM_FORMAT_MOD_LINEAR));
}
