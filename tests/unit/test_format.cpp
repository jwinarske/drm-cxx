// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "core/format.hpp"

#include <drm_fourcc.h>

#include <gtest/gtest.h>

TEST(FormatTest, KnownFormatsReturnCorrectName) {
  EXPECT_EQ(drm::format_name(DRM_FORMAT_XRGB8888), "XRGB8888");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_ARGB8888), "ARGB8888");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_RGB565), "RGB565");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_NV12), "NV12");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_ABGR16161616F), "ABGR16161616F");
}

// HDR-relevant pixel formats: 10/12/16-bit YUV.
TEST(FormatTest, HighBitDepthYuvFormatsHaveNames) {
  EXPECT_EQ(drm::format_name(DRM_FORMAT_P010), "P010");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_P012), "P012");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_P016), "P016");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_NV15), "NV15");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_NV20), "NV20");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_Y210), "Y210");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_Y212), "Y212");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_Y216), "Y216");
}

TEST(FormatTest, UnknownFormatReturnsUnknown) {
  EXPECT_EQ(drm::format_name(0), "unknown");
  EXPECT_EQ(drm::format_name(0xDEADBEEF), "unknown");
}

TEST(FormatTest, KnownFormatsReturnCorrectBpp) {
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_C8), 8U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_RGB565), 16U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_RGB888), 24U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_XRGB8888), 32U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_ARGB8888), 32U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_XRGB2101010), 32U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_ARGB16161616F), 64U);
}

// Packed YUV 4:2:2 with u16 samples — 4 components per 2 luma
// pixels = 64 bits / 2 pixels = 32 bpp.
TEST(FormatTest, PackedHighBitDepthYuvIs32Bpp) {
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_Y210), 32U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_Y212), 32U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_Y216), 32U);
}

// Planar formats (NV-family + P010 / P012 / P016) deliberately
// return 0 — per-plane bpp varies (Y plane vs UV plane), so a
// single image-level scalar is misleading. Per-plane callers
// should use libdrm's drm_get_format_info instead.
TEST(FormatTest, PlanarFormatsReturnZeroBpp) {
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_NV12), 0U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_NV15), 0U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_NV16), 0U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_NV20), 0U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_P010), 0U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_P012), 0U);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_P016), 0U);
}

TEST(FormatTest, UnknownFormatReturnsZeroBpp) {
  EXPECT_EQ(drm::format_bpp(0), 0U);
  EXPECT_EQ(drm::format_bpp(0xDEADBEEF), 0U);
}
