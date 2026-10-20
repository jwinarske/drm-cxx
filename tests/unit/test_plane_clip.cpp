// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "planes/plane_clip.hpp"

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <cstdint>
#include <gtest/gtest.h>

using drm::planes::detail::clip_applies;
using drm::planes::detail::clip_leading;
using drm::planes::detail::PlaneSpan;

namespace {

constexpr std::uint64_t fp(std::uint64_t v) {
  return v << 16U;
}

}  // namespace

TEST(PlaneClip, OnScreenSpanIsUntouched) {
  PlaneSpan s{0, 64, fp(4), fp(64)};
  EXPECT_FALSE(clip_leading(s));
  EXPECT_EQ(s.dst, 0);
  EXPECT_EQ(s.dst_len, 64U);
  EXPECT_EQ(s.src, fp(4));
  EXPECT_EQ(s.src_len, fp(64));
}

TEST(PlaneClip, UnscaledClipTrimsTheSourceByTheSameAmount) {
  PlaneSpan s{-16, 64, fp(0), fp(64)};
  EXPECT_TRUE(clip_leading(s));
  EXPECT_EQ(s.dst, 0);
  EXPECT_EQ(s.dst_len, 48U);
  EXPECT_EQ(s.src, fp(16));
  EXPECT_EQ(s.src_len, fp(48));
}

TEST(PlaneClip, UpscaledClipRoundsTheSourceUp) {
  // 10 px shown over 30: 655360 * 29 / 30 = 633514.67, rounded up.
  PlaneSpan s{-1, 30, fp(0), fp(10)};
  EXPECT_TRUE(clip_leading(s));
  EXPECT_EQ(s.dst, 0);
  EXPECT_EQ(s.dst_len, 29U);
  EXPECT_EQ(s.src_len, 633515U);
  EXPECT_EQ(s.src + s.src_len, fp(10)) << "far edge stays put";
}

TEST(PlaneClip, DownscaledClipRoundsTheSourceDown) {
  // 100 px shown over 30: 6553600 * 29 / 30 = 6335146.67, rounded down.
  PlaneSpan s{-1, 30, fp(8), fp(100)};
  EXPECT_TRUE(clip_leading(s));
  EXPECT_EQ(s.dst_len, 29U);
  EXPECT_EQ(s.src_len, 6335146U);
  EXPECT_EQ(s.src + s.src_len, fp(108)) << "far edge stays put";
}

TEST(PlaneClip, SpanEntirelyOffScreenIsLeftAlone) {
  PlaneSpan s{-64, 64, fp(0), fp(64)};
  EXPECT_FALSE(clip_leading(s));
  EXPECT_EQ(s.dst, -64);
  EXPECT_EQ(s.dst_len, 64U);
  EXPECT_EQ(s.src_len, fp(64));
}

TEST(PlaneClip, SpanBeyond32BitsIsLeftAlone) {
  PlaneSpan s{-1, 30, fp(0), fp(0x10000)};
  EXPECT_FALSE(clip_leading(s));
  EXPECT_EQ(s.dst, -1);
  EXPECT_EQ(s.src_len, fp(0x10000));
}

TEST(PlaneClip, AppliesOnlyUnrotatedAndUnsubsampled) {
  EXPECT_TRUE(clip_applies(0, DRM_FORMAT_ARGB8888));
  EXPECT_TRUE(clip_applies(DRM_MODE_ROTATE_0, DRM_FORMAT_XRGB8888));
  EXPECT_FALSE(clip_applies(DRM_MODE_ROTATE_90, DRM_FORMAT_ARGB8888));
  EXPECT_FALSE(clip_applies(DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X, DRM_FORMAT_ARGB8888));
  EXPECT_FALSE(clip_applies(0, DRM_FORMAT_NV12));
  EXPECT_FALSE(clip_applies(0, DRM_FORMAT_YUYV));
  EXPECT_TRUE(clip_applies(0, DRM_FORMAT_NV24)) << "4:4:4 has no subsampling";
}
