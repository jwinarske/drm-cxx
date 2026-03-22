// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "modeset/mode.hpp"

#include <xf86drmMode.h>

#include <cstring>
#include <gtest/gtest.h>

namespace {

drmModeModeInfo make_mode(uint16_t w, uint16_t h, uint32_t refresh, uint32_t type = 0,
                          uint32_t flags = 0) {
  drmModeModeInfo m{};
  m.hdisplay = w;
  m.vdisplay = h;
  m.vrefresh = refresh;
  m.type = type;
  m.flags = flags;
  m.clock = w * h * refresh / 1000;  // approximate
  std::snprintf(m.name, sizeof(m.name), "%ux%u", w, h);
  return m;
}

}  // namespace

TEST(ModeInfoTest, Accessors) {
  auto m = make_mode(1920, 1080, 60, DRM_MODE_TYPE_PREFERRED);
  drm::ModeInfo info{.drm_mode = m};

  EXPECT_EQ(info.width(), 1920u);
  EXPECT_EQ(info.height(), 1080u);
  EXPECT_EQ(info.refresh(), 60u);
  EXPECT_TRUE(info.preferred());
  EXPECT_FALSE(info.interlaced());
}

TEST(ModeInfoTest, InterlacedFlag) {
  auto m = make_mode(1920, 1080, 30, 0, DRM_MODE_FLAG_INTERLACE);
  drm::ModeInfo info{.drm_mode = m};
  EXPECT_TRUE(info.interlaced());
}

TEST(ModeSelectionTest, EmptyModesReturnsError) {
  std::span<const drmModeModeInfo> empty;
  auto result = drm::select_preferred_mode(empty);
  EXPECT_FALSE(result.has_value());
}

TEST(ModeSelectionTest, PreferredModeIsSelected) {
  drmModeModeInfo modes[] = {
      make_mode(1280, 720, 60),
      make_mode(1920, 1080, 60, DRM_MODE_TYPE_PREFERRED),
      make_mode(3840, 2160, 30),
  };

  auto result = drm::select_preferred_mode(modes);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->width(), 1920u);
  EXPECT_EQ(result->height(), 1080u);
  EXPECT_TRUE(result->preferred());
}

TEST(ModeSelectionTest, FallbackToHighestResolution) {
  drmModeModeInfo modes[] = {
      make_mode(1280, 720, 60),
      make_mode(1920, 1080, 60),
      make_mode(3840, 2160, 30),
  };

  auto result = drm::select_preferred_mode(modes);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->width(), 3840u);
  EXPECT_EQ(result->height(), 2160u);
}

TEST(ModeSelectionTest, SelectModeExactMatch) {
  drmModeModeInfo modes[] = {
      make_mode(1280, 720, 60),
      make_mode(1920, 1080, 60),
      make_mode(1920, 1080, 144),
      make_mode(3840, 2160, 60),
  };

  auto result = drm::select_mode(modes, 1920, 1080);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->width(), 1920u);
  EXPECT_EQ(result->height(), 1080u);
  // Should pick highest refresh at that resolution
  EXPECT_EQ(result->refresh(), 144u);
}

TEST(ModeSelectionTest, SelectModeWithRefresh) {
  drmModeModeInfo modes[] = {
      make_mode(1920, 1080, 60),
      make_mode(1920, 1080, 144),
  };

  auto result = drm::select_mode(modes, 1920, 1080, 60);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->refresh(), 60u);
}

TEST(ModeSelectionTest, SelectModeClosestMatch) {
  drmModeModeInfo modes[] = {
      make_mode(1280, 720, 60),
      make_mode(1920, 1080, 60),
      make_mode(2560, 1440, 60),
  };

  // Asking for 1800x1000 — closest is 1920x1080 (dw=120,dh=80 → 20800)
  // vs 1280x720 (dw=520,dh=280 → 348800) and 2560x1440 (dw=760,dh=440 → 771200)
  auto result = drm::select_mode(modes, 1800, 1000);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->width(), 1920u);
  EXPECT_EQ(result->height(), 1080u);
}

TEST(ModeSelectionTest, GetAllModes) {
  drmModeModeInfo modes[] = {
      make_mode(1280, 720, 60),
      make_mode(1920, 1080, 60),
  };

  auto all = drm::get_all_modes(modes);
  EXPECT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].width(), 1280u);
  EXPECT_EQ(all[1].width(), 1920u);
}

TEST(ModeSelectionTest, SkipsInterlacedInSelectMode) {
  drmModeModeInfo modes[] = {
      make_mode(1920, 1080, 60, 0, DRM_MODE_FLAG_INTERLACE),
      make_mode(1280, 720, 60),
  };

  auto result = drm::select_mode(modes, 1920, 1080);
  ASSERT_TRUE(result.has_value());
  // Should skip interlaced 1080 and pick 720
  EXPECT_EQ(result->width(), 1280u);
}
