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

// Single-V4L2-plane semi-planar 4:2:0: Y plane then interleaved UV. NV12 (8-bit)
// plus the 16-bit-per-sample HDR variants P010/P012/P016 share the storage shape
// -- only the per-sample width differs, and that is already baked into
// bytesperline by V4L2 -- so all four split the same way into 2 DRM planes.
[[nodiscard]] bool is_semiplanar_420(std::uint32_t drm_fourcc) noexcept {
  return drm_fourcc == DRM_FORMAT_NV12 || drm_fourcc == DRM_FORMAT_P010 ||
         drm_fourcc == DRM_FORMAT_P012 || drm_fourcc == DRM_FORMAT_P016;
}

// Single-V4L2-plane fully-planar 4:2:0: Y, then two quarter-resolution chroma
// planes. YUV420 (I420) orders them Cb,Cr; YVU420 (YV12) orders them Cr,Cb --
// both chroma planes are the same size, so the offset math is identical.
[[nodiscard]] bool is_planar_420(std::uint32_t drm_fourcc) noexcept {
  return drm_fourcc == DRM_FORMAT_YUV420 || drm_fourcc == DRM_FORMAT_YVU420;
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
  if (num_v4l2 == 1 && (is_semiplanar_420(drm_fourcc) || is_planar_420(drm_fourcc))) {
    const std::uint32_t bpl = plane_bpl(cap_fmt, is_mplane, 0);
    if (bpl == 0 || bpl > k_max_bytes_per_line) {
      return std::make_error_code(std::errc::invalid_argument);
    }
    // u64-promoted so a hostile echo can't wrap; AddFB2 offsets are u32.
    const auto y_size = static_cast<std::uint64_t>(bpl) * h;

    if (is_semiplanar_420(drm_fourcc)) {
      // Y at offset 0, interleaved UV (full luma stride) at bpl*h.
      if (y_size > std::numeric_limits<std::uint32_t>::max()) {
        return std::make_error_code(std::errc::value_too_large);
      }
      out.num_drm_planes = 2;
      out.pitch.at(0) = bpl;
      out.pitch.at(1) = bpl;
      out.offset.at(0) = 0;
      out.offset.at(1) = static_cast<std::uint32_t>(y_size);
      out.v4l2_plane_idx.at(0) = 0;
      out.v4l2_plane_idx.at(1) = 0;
      return {};
    }

    // Planar 4:2:0: two chroma planes, each stride bpl/2 and height ceil(h/2).
    // 4:2:0 needs an even luma stride so it halves cleanly; odd is malformed.
    if ((bpl & 1U) != 0U) {
      return std::make_error_code(std::errc::invalid_argument);
    }
    const std::uint64_t chroma_bpl = bpl / 2;
    const std::uint64_t chroma_h = (static_cast<std::uint64_t>(h) + 1) / 2;  // DIV_ROUND_UP(h, 2)
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
