// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::detail::derive_drm_plane_layout -- the shared
// V4L2-CAPTURE-echo -> DRM AddFB2 plane-layout mapping used by the camera and
// decoder sources. Pure logic; no device needed.

#include <drm-cxx/scene/v4l2_plane_layout.hpp>

#include <drm_fourcc.h>

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <linux/videodev2.h>
#include <system_error>

namespace {

using drm::scene::detail::derive_drm_plane_layout;
using drm::scene::detail::DrmPlaneLayout;

// Single-planar (non-MPLANE) CAPTURE echo.
v4l2_format single_plane(std::uint32_t width, std::uint32_t height,
                         std::uint32_t bytesperline) noexcept {
  v4l2_format f{};
  f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  f.fmt.pix.width = width;
  f.fmt.pix.height = height;
  f.fmt.pix.bytesperline = bytesperline;
  return f;
}

// MPLANE CAPTURE echo with `n` explicit V4L2 planes.
v4l2_format mplane(std::uint32_t width, std::uint32_t height, std::uint32_t n,
                   const std::array<std::uint32_t, 4>& bpl) noexcept {
  v4l2_format f{};
  f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  f.fmt.pix_mp.width = width;
  f.fmt.pix_mp.height = height;
  f.fmt.pix_mp.num_planes = static_cast<std::uint8_t>(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    f.fmt.pix_mp.plane_fmt[i].bytesperline = bpl.at(i);
  }
  return f;
}

TEST(V4l2PlaneLayout, SemiplanarNv12SinglePlaneSplitsToTwo) {
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(single_plane(1920, 1080, 1920), false, DRM_FORMAT_NV12, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 2U);
  EXPECT_EQ(out.pitch.at(0), 1920U);
  EXPECT_EQ(out.pitch.at(1), 1920U);
  EXPECT_EQ(out.offset.at(0), 0U);
  EXPECT_EQ(out.offset.at(1), 1920U * 1080U);  // Y size = bpl * h
  EXPECT_EQ(out.v4l2_plane_idx.at(0), 0U);
  EXPECT_EQ(out.v4l2_plane_idx.at(1), 0U);
}

TEST(V4l2PlaneLayout, SemiplanarP010UsesByteStride) {
  // P010's bytesperline is already 2x the sample count, so the offset math is
  // identical in shape to NV12 -- the chroma offset is just bpl * h.
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(single_plane(1920, 1080, 3840), false, DRM_FORMAT_P010, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 2U);
  EXPECT_EQ(out.offset.at(1), 3840U * 1080U);
}

TEST(V4l2PlaneLayout, Planar420SinglePlaneSplitsToThree) {
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(single_plane(1920, 1080, 1920), false, DRM_FORMAT_YUV420, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 3U);
  EXPECT_EQ(out.pitch.at(0), 1920U);
  EXPECT_EQ(out.pitch.at(1), 960U);  // chroma stride = bpl / 2
  EXPECT_EQ(out.pitch.at(2), 960U);
  EXPECT_EQ(out.offset.at(0), 0U);
  EXPECT_EQ(out.offset.at(1), 1920U * 1080U);                    // after Y
  EXPECT_EQ(out.offset.at(2), (1920U * 1080U) + (960U * 540U));  // after Y + Cb
  EXPECT_EQ(out.v4l2_plane_idx.at(2), 0U);
}

TEST(V4l2PlaneLayout, PlanarYvu420SameOffsetsAsYuv420) {
  // YV12 swaps the Cr/Cb order but both chroma planes are the same size, so the
  // derived offsets are identical to I420.
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(single_plane(1280, 720, 1280), false, DRM_FORMAT_YVU420, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 3U);
  EXPECT_EQ(out.offset.at(1), 1280U * 720U);
  EXPECT_EQ(out.offset.at(2), (1280U * 720U) + (640U * 360U));
}

TEST(V4l2PlaneLayout, Planar420OddHeightRoundsChromaUp) {
  // ceil(h/2): for h=3 the chroma plane is 2 rows, so plane 2 sits at
  // bpl*h + (bpl/2)*2.
  DrmPlaneLayout out{};
  const auto ec = derive_drm_plane_layout(single_plane(4, 3, 4), false, DRM_FORMAT_YUV420, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.offset.at(2), (4U * 3U) + (2U * 2U));
}

TEST(V4l2PlaneLayout, SemiplanarNv16IsFullHeightChromaButSameLayout) {
  // NV16 is 4:2:2: the chroma plane is full-height, but it still starts at bpl*h
  // with the full luma stride, so the derived pitch/offset match NV12's. AddFB2
  // derives the chroma height from the format itself.
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(single_plane(1280, 720, 1280), false, DRM_FORMAT_NV16, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 2U);
  EXPECT_EQ(out.pitch.at(1), 1280U);
  EXPECT_EQ(out.offset.at(1), 1280U * 720U);
}

TEST(V4l2PlaneLayout, Planar422FullHeightChroma) {
  // YUV422: two chroma planes at stride bpl/2 and FULL height (vsub == 1), so
  // plane 2 sits at bpl*h + (bpl/2)*h.
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(single_plane(1280, 720, 1280), false, DRM_FORMAT_YUV422, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 3U);
  EXPECT_EQ(out.pitch.at(1), 640U);
  EXPECT_EQ(out.pitch.at(2), 640U);
  EXPECT_EQ(out.offset.at(1), 1280U * 720U);
  EXPECT_EQ(out.offset.at(2), (1280U * 720U) + (640U * 720U));  // chroma full height
}

TEST(V4l2PlaneLayout, PlanarYvu422SameOffsetsAsYuv422) {
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(single_plane(640, 480, 640), false, DRM_FORMAT_YVU422, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 3U);
  EXPECT_EQ(out.offset.at(2), (640U * 480U) + (320U * 480U));
}

TEST(V4l2PlaneLayout, Planar444FullResolutionChroma) {
  // YUV444: no subsampling -- both chroma planes are full width AND full height
  // (stride bpl, height h), so plane 1 is at bpl*h and plane 2 at 2*bpl*h.
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(single_plane(1280, 720, 1280), false, DRM_FORMAT_YUV444, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 3U);
  EXPECT_EQ(out.pitch.at(0), 1280U);
  EXPECT_EQ(out.pitch.at(1), 1280U);  // full-width chroma
  EXPECT_EQ(out.pitch.at(2), 1280U);
  EXPECT_EQ(out.offset.at(1), 1280U * 720U);
  EXPECT_EQ(out.offset.at(2), 2U * 1280U * 720U);
}

TEST(V4l2PlaneLayout, Planar444OddStrideAllowed) {
  // 4:4:4 chroma stride equals the luma stride (no halving), so an odd bpl is
  // fine -- unlike 4:2:0 / 4:2:2 which need an even luma stride.
  DrmPlaneLayout out{};
  const auto ec = derive_drm_plane_layout(single_plane(3, 2, 3), false, DRM_FORMAT_YVU444, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 3U);
  EXPECT_EQ(out.pitch.at(1), 3U);
  EXPECT_EQ(out.offset.at(2), 2U * 3U * 2U);
}

TEST(V4l2PlaneLayout, Planar420OddStrideRejected) {
  DrmPlaneLayout out{};
  const auto ec = derive_drm_plane_layout(single_plane(3, 2, 3), false, DRM_FORMAT_YUV420, out);
  EXPECT_EQ(ec, std::errc::invalid_argument);
}

TEST(V4l2PlaneLayout, PackedYuyvIsSingleDrmPlane) {
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(single_plane(1280, 720, 2560), false, DRM_FORMAT_YUYV, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 1U);
  EXPECT_EQ(out.pitch.at(0), 2560U);
  EXPECT_EQ(out.offset.at(0), 0U);
}

TEST(V4l2PlaneLayout, MplaneNv12TwoPlanesMapOneToOne) {
  // An MPLANE decoder that hands back two distinct CAPTURE planes maps 1:1,
  // each DRM plane importing its own V4L2 plane fd.
  DrmPlaneLayout out{};
  const auto ec = derive_drm_plane_layout(mplane(1920, 1080, 2, {1920, 1920, 0, 0}), true,
                                          DRM_FORMAT_NV12, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 2U);
  EXPECT_EQ(out.offset.at(1), 0U);  // separate buffer, not an offset into plane 0
  EXPECT_EQ(out.v4l2_plane_idx.at(0), 0U);
  EXPECT_EQ(out.v4l2_plane_idx.at(1), 1U);
}

TEST(V4l2PlaneLayout, MplaneSinglePlaneNv12StillSplits) {
  // Some MPLANE decoders pack the whole frame into CAPTURE plane 0 (num_planes
  // == 1). NV12 still needs two DRM planes -> split by offset math.
  DrmPlaneLayout out{};
  const auto ec =
      derive_drm_plane_layout(mplane(1920, 1080, 1, {1920, 0, 0, 0}), true, DRM_FORMAT_NV12, out);
  ASSERT_FALSE(ec);
  EXPECT_EQ(out.num_drm_planes, 2U);
  EXPECT_EQ(out.offset.at(1), 1920U * 1080U);
}

TEST(V4l2PlaneLayout, RejectsDegenerateDims) {
  DrmPlaneLayout out{};
  EXPECT_EQ(derive_drm_plane_layout(single_plane(0, 0, 0), false, DRM_FORMAT_YUV420, out),
            std::errc::invalid_argument);
  EXPECT_EQ(derive_drm_plane_layout(single_plane(64, 100000, 64), false, DRM_FORMAT_YUV420, out),
            std::errc::invalid_argument);
  EXPECT_EQ(derive_drm_plane_layout(single_plane(64, 64, 200000), false, DRM_FORMAT_YUV420, out),
            std::errc::invalid_argument);
}

}  // namespace
