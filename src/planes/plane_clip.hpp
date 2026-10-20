// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Private (not installed): clip a plane's destination at the CRTC's top and
// left edges.
//
// Core KMS accepts a negative CRTC_X/CRTC_Y and clips the source to match, but
// some drivers check the unclipped destination and refuse it (the downstream
// SDE driver: "invalid vertical/horizontal destination"). Clipping before the
// write gives the same scanout everywhere. The source is trimmed the way
// drm_rect_clip_scaled() does it, so the scale factor keeps its direction and
// the far edge stays put.

#pragma once

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <cstdint>

namespace drm::planes::detail {

/// One axis of a plane: destination origin and length in CRTC pixels, source
/// origin and length in 16.16 fixed point.
struct PlaneSpan {
  std::int64_t dst;
  std::uint64_t dst_len;
  std::uint64_t src;
  std::uint64_t src_len;
};

/// Clips `s` at 0. A span entirely before 0 (or empty) is left as is: there
/// is nothing visible to keep, and the caller's own off-screen handling applies.
/// So is one whose lengths overflow the kernel's 32-bit properties; the driver
/// refuses it either way, and the bound keeps the product below in 64 bits.
/// Returns whether it clipped.
[[nodiscard]] inline bool clip_leading(PlaneSpan& s) noexcept {
  if (s.dst >= 0 || s.dst_len == 0 || s.dst_len > UINT32_MAX || s.src_len > UINT32_MAX) {
    return false;
  }
  const auto clip = static_cast<std::uint64_t>(-s.dst);
  if (clip >= s.dst_len) {
    return false;
  }
  const std::uint64_t kept = s.dst_len - clip;
  const std::uint64_t scaled = s.src_len * kept;
  // Round toward 1:1 so clipping never turns upscaling into downscaling or
  // the reverse.
  const std::uint64_t new_src_len =
      s.src_len < (s.dst_len << 16U) ? (scaled + s.dst_len - 1) / s.dst_len : scaled / s.dst_len;
  s.src += s.src_len - new_src_len;
  s.src_len = new_src_len;
  s.dst = 0;
  s.dst_len = kept;
  return true;
}

/// Formats whose chroma is subsampled. A clip can land the source origin on an
/// odd luma position, which these cannot start at, so they are left unclipped.
[[nodiscard]] constexpr bool chroma_subsampled(std::uint32_t fourcc) noexcept {
  switch (fourcc) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV61:
    case DRM_FORMAT_NV15:
#ifdef DRM_FORMAT_NV20
    case DRM_FORMAT_NV20:
#endif
    case DRM_FORMAT_P010:
    case DRM_FORMAT_P012:
    case DRM_FORMAT_P016:
    case DRM_FORMAT_P210:
    case DRM_FORMAT_YUV410:
    case DRM_FORMAT_YVU410:
    case DRM_FORMAT_YUV411:
    case DRM_FORMAT_YVU411:
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YVU420:
    case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YVU422:
    case DRM_FORMAT_YUYV:
    case DRM_FORMAT_YVYU:
    case DRM_FORMAT_UYVY:
    case DRM_FORMAT_VYUY:
    case DRM_FORMAT_Y210:
    case DRM_FORMAT_Y212:
    case DRM_FORMAT_Y216:
      return true;
    default:
      return false;
  }
}

/// Whether a plane with this rotation and format may be clipped. A rotated or
/// reflected plane reads its source from a different edge, so the trim would
/// land on the wrong side.
[[nodiscard]] constexpr bool clip_applies(std::uint64_t rotation, std::uint32_t fourcc) noexcept {
  return (rotation == 0 || rotation == DRM_MODE_ROTATE_0) && !chroma_subsampled(fourcc);
}

}  // namespace drm::planes::detail
