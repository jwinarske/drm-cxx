// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// drm-cxx/scene/v4l2_plane_layout.cpp

#include "v4l2_plane_layout.hpp"

#include <drm_fourcc.h>

#include <cstdint>
#include <limits>
#include <linux/videodev2.h>
#include <system_error>

namespace drm::scene::detail {
namespace {

// Single-V4L2-plane semi-planar YUV: Y plane then one interleaved chroma plane,
// starting at bytesperline*height -> 2 DRM planes. The chroma plane's height
// follows from vertical subsampling (half for 4:2:0, full for 4:2:2/4:4:4), which
// AddFB2 derives itself. The chroma *stride* is the luma stride for the
// horizontally-subsampled formats (4:2:0 NV12 + 16-bit P010/P012/P016, 4:2:2
// NV16/NV61) but twice it for full-resolution 4:4:4 (NV24/NV42) -- see
// semiplanar_chroma_pitch(). Covers all of those.
[[nodiscard]] bool is_semiplanar_yuv(std::uint32_t drm_fourcc) noexcept {
  return drm_fourcc == DRM_FORMAT_NV12 || drm_fourcc == DRM_FORMAT_P010 ||
         drm_fourcc == DRM_FORMAT_P012 || drm_fourcc == DRM_FORMAT_P016 ||
         drm_fourcc == DRM_FORMAT_NV16 || drm_fourcc == DRM_FORMAT_NV61 ||
         drm_fourcc == DRM_FORMAT_NV24 || drm_fourcc == DRM_FORMAT_NV42;
}

// Stride of the interleaved chroma plane for a semi-planar format, given the luma
// stride `bpl`. 4:4:4 (NV24/NV42) carries a full-resolution CbCr pair per column,
// so its chroma row is twice the luma row; the horizontally-subsampled formats
// (4:2:0 / 4:2:2, and the P0xx byte-doublings) match the luma stride.
[[nodiscard]] std::uint32_t semiplanar_chroma_pitch(std::uint32_t drm_fourcc,
                                                    std::uint32_t bpl) noexcept {
  const bool full_width = (drm_fourcc == DRM_FORMAT_NV24 || drm_fourcc == DRM_FORMAT_NV42);
  return full_width ? (bpl * 2U) : bpl;
}

// Chroma subsampling of the single-V4L2-plane fully-planar YUV formats -> Y plus
// two equal-size chroma planes (3 DRM planes), each at stride bpl/hsub and height
// ceil(h/vsub). {0,0} for a format that is not planar YUV. The Cb/Cr order
// (I420 vs YV12) doesn't change the offsets since both chroma planes are equal.
struct ChromaSubsampling {
  unsigned hsub;  // horizontal (2 -> half-width chroma, 1 -> full-width)
  unsigned vsub;  // vertical   (2 -> half-height chroma, 1 -> full-height)
};

[[nodiscard]] ChromaSubsampling planar_yuv_subsampling(std::uint32_t drm_fourcc) noexcept {
  if (drm_fourcc == DRM_FORMAT_YUV420 || drm_fourcc == DRM_FORMAT_YVU420) {
    return {2, 2};  // 4:2:0
  }
  if (drm_fourcc == DRM_FORMAT_YUV422 || drm_fourcc == DRM_FORMAT_YVU422) {
    return {2, 1};  // 4:2:2
  }
  if (drm_fourcc == DRM_FORMAT_YUV444 || drm_fourcc == DRM_FORMAT_YVU444) {
    return {1, 1};  // 4:4:4 -- full-resolution chroma
  }
  return {0, 0};
}

[[nodiscard]] std::uint32_t plane_bpl(const v4l2_format& f, bool is_mplane,
                                      std::uint32_t idx) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  return is_mplane ? f.fmt.pix_mp.plane_fmt[idx].bytesperline : f.fmt.pix.bytesperline;
}

}  // namespace

std::error_code derive_drm_plane_layout(const v4l2_format& cap_fmt, bool is_mplane,
                                        std::uint32_t drm_fourcc, DrmPlaneLayout& out) noexcept {
  const std::uint32_t h = is_mplane ? cap_fmt.fmt.pix_mp.height : cap_fmt.fmt.pix.height;
  const std::uint32_t num_v4l2 = is_mplane ? cap_fmt.fmt.pix_mp.num_planes : 1U;
  if (h == 0 || h > k_max_image_dim || num_v4l2 == 0 || num_v4l2 > k_drm_max_planes) {
    return std::make_error_code(std::errc::invalid_argument);
  }

  // A single V4L2 plane that holds a whole multi-plane frame (single-planar API,
  // or an MPLANE decoder that packs everything into CAPTURE plane 0 -- e.g. the
  // Pi bcm2835-codec). DRM still needs 2 or 3 plane handles, so split by offset
  // math; all DRM planes import V4L2 plane 0 (the kernel dedups the prime handle).
  const bool semiplanar = is_semiplanar_yuv(drm_fourcc);
  const ChromaSubsampling planar = planar_yuv_subsampling(drm_fourcc);
  if (num_v4l2 == 1 && (semiplanar || planar.hsub != 0)) {
    const std::uint32_t bpl = plane_bpl(cap_fmt, is_mplane, 0);
    if (bpl == 0 || bpl > k_max_bytes_per_line) {
      return std::make_error_code(std::errc::invalid_argument);
    }
    // u64-promoted so a hostile echo can't wrap; AddFB2 offsets are u32.
    const auto y_size = static_cast<std::uint64_t>(bpl) * h;

    if (semiplanar) {
      // Y at offset 0, interleaved chroma at bpl*h; chroma stride is the luma
      // stride except for full-resolution 4:4:4 (NV24/NV42), which doubles it.
      if (y_size > std::numeric_limits<std::uint32_t>::max()) {
        return std::make_error_code(std::errc::value_too_large);
      }
      out.num_drm_planes = 2;
      out.pitch.at(0) = bpl;
      out.pitch.at(1) = semiplanar_chroma_pitch(drm_fourcc, bpl);
      out.offset.at(0) = 0;
      out.offset.at(1) = static_cast<std::uint32_t>(y_size);
      out.v4l2_plane_idx.at(0) = 0;
      out.v4l2_plane_idx.at(1) = 0;
      return {};
    }

    // Planar YUV: two equal-size chroma planes, each at stride bpl/hsub and
    // height ceil(h/vsub) -- 4:2:0 {2,2}, 4:2:2 {2,1}, 4:4:4 {1,1}. A halved luma
    // stride (hsub == 2) needs an even bpl so it divides cleanly; odd is malformed.
    if (planar.hsub == 2 && (bpl & 1U) != 0U) {
      return std::make_error_code(std::errc::invalid_argument);
    }
    const std::uint64_t chroma_bpl = bpl / planar.hsub;
    const std::uint64_t chroma_h =
        (static_cast<std::uint64_t>(h) + planar.vsub - 1) / planar.vsub;  // DIV_ROUND_UP(h, vsub)
    const std::uint64_t chroma_size = chroma_bpl * chroma_h;
    const std::uint64_t v_offset = y_size + chroma_size;
    // Largest offset (plane 2) plus its own extent must stay inside AddFB2's u32.
    if (v_offset + chroma_size > std::numeric_limits<std::uint32_t>::max()) {
      return std::make_error_code(std::errc::value_too_large);
    }
    out.num_drm_planes = 3;
    out.pitch.at(0) = bpl;
    out.pitch.at(1) = static_cast<std::uint32_t>(chroma_bpl);
    out.pitch.at(2) = static_cast<std::uint32_t>(chroma_bpl);
    out.offset.at(0) = 0;
    out.offset.at(1) = static_cast<std::uint32_t>(y_size);
    out.offset.at(2) = static_cast<std::uint32_t>(v_offset);
    out.v4l2_plane_idx.at(0) = 0;
    out.v4l2_plane_idx.at(1) = 0;
    out.v4l2_plane_idx.at(2) = 0;
    return {};
  }

  // Default: 1 DRM plane per V4L2 plane. Covers MPLANE with a matching plane
  // count, single-plane packed (YUYV, RGB), and any single-DRM-plane format.
  out.num_drm_planes = num_v4l2;
  for (std::uint32_t i = 0; i < num_v4l2; ++i) {
    const std::uint32_t bpl = plane_bpl(cap_fmt, is_mplane, i);
    if (bpl == 0 || bpl > k_max_bytes_per_line) {
      return std::make_error_code(std::errc::invalid_argument);
    }
    out.pitch.at(i) = bpl;
    out.offset.at(i) = 0;
    out.v4l2_plane_idx.at(i) = static_cast<std::uint8_t>(i);
  }
  return {};
}

}  // namespace drm::scene::detail
