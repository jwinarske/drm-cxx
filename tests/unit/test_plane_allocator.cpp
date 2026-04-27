// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

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

TEST(PlaneCapabilitiesTest, SupportsFormatModifierFallsBackWhenInFormatsAbsent) {
  // Driver doesn't expose IN_FORMATS — only the bare format list. The
  // fallback path accepts LINEAR / INVALID against advertised formats and
  // rejects everything else.
  drm::planes::PlaneCapabilities caps;
  caps.formats = {DRM_FORMAT_XRGB8888};
  caps.has_format_modifiers = false;

  EXPECT_TRUE(caps.supports_format_modifier(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR));
  EXPECT_TRUE(caps.supports_format_modifier(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID));

  // Format the plane doesn't carry — even with a linear modifier.
  EXPECT_FALSE(caps.supports_format_modifier(DRM_FORMAT_RGB565, DRM_FORMAT_MOD_LINEAR));

  // Non-trivial modifier without IN_FORMATS evidence: rejected.
  constexpr uint64_t k_afbc_like = (1ULL << 56) | 1;
  EXPECT_FALSE(caps.supports_format_modifier(DRM_FORMAT_XRGB8888, k_afbc_like));
}

TEST(PlaneCapabilitiesTest, SupportsFormatModifierFromInFormatsBlob) {
  // Driver exposed IN_FORMATS — the (format, modifier) pair list is
  // authoritative. Formats not listed are rejected even if `formats`
  // happens to carry them; a non-trivial modifier is accepted only when
  // the exact pair is in the IN_FORMATS list.
  constexpr uint64_t k_afbc_like = (1ULL << 56) | 1;
  drm::planes::PlaneCapabilities caps;
  caps.formats = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
  caps.format_modifiers = {
      {DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR},
      {DRM_FORMAT_XRGB8888, k_afbc_like},
      {DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR},
  };
  // PlaneCapabilities::format_modifiers must be sorted by format ascending —
  // production code's lookup uses lower_bound. parse_in_formats_blob sorts
  // for callers; tests that populate the field directly must mirror that.
  std::sort(caps.format_modifiers.begin(), caps.format_modifiers.end());
  caps.has_format_modifiers = true;

  // Linear pair listed.
  EXPECT_TRUE(caps.supports_format_modifier(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR));
  // INVALID treated as LINEAR-equivalent.
  EXPECT_TRUE(caps.supports_format_modifier(DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID));
  // Vendor tiling listed.
  EXPECT_TRUE(caps.supports_format_modifier(DRM_FORMAT_XRGB8888, k_afbc_like));

  // ARGB only listed for LINEAR — vendor tiling on ARGB rejected.
  EXPECT_TRUE(caps.supports_format_modifier(DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR));
  EXPECT_FALSE(caps.supports_format_modifier(DRM_FORMAT_ARGB8888, k_afbc_like));

  // Format absent from the pair list — rejected even if `formats` lists it.
  caps.formats.push_back(DRM_FORMAT_RGB565);
  EXPECT_FALSE(caps.supports_format_modifier(DRM_FORMAT_RGB565, DRM_FORMAT_MOD_LINEAR));
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
