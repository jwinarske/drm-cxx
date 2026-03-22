// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <drm_fourcc.h>

#include "core/format.hpp"

TEST(FormatTest, KnownFormatsReturnCorrectName) {
  EXPECT_EQ(drm::format_name(DRM_FORMAT_XRGB8888), "XRGB8888");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_ARGB8888), "ARGB8888");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_RGB565), "RGB565");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_NV12), "NV12");
  EXPECT_EQ(drm::format_name(DRM_FORMAT_ABGR16161616F), "ABGR16161616F");
}

TEST(FormatTest, UnknownFormatReturnsUnknown) {
  EXPECT_EQ(drm::format_name(0), "unknown");
  EXPECT_EQ(drm::format_name(0xDEADBEEF), "unknown");
}

TEST(FormatTest, KnownFormatsReturnCorrectBpp) {
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_C8), 8u);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_RGB565), 16u);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_RGB888), 24u);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_XRGB8888), 32u);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_ARGB8888), 32u);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_XRGB2101010), 32u);
  EXPECT_EQ(drm::format_bpp(DRM_FORMAT_ARGB16161616F), 64u);
}

TEST(FormatTest, UnknownFormatReturnsZeroBpp) {
  EXPECT_EQ(drm::format_bpp(0), 0u);
  EXPECT_EQ(drm::format_bpp(0xDEADBEEF), 0u);
}
