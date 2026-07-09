// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// drm-cxx/scene/v4l2_plane_layout.hpp
//
// Shared V4L2-CAPTURE-echo -> DRM-AddFB2 plane-layout derivation, used by both
// the V4L2 camera and stateful-decoder sources. A single V4L2 CAPTURE plane can
// back multiple DRM planes (offset math), so this decides the per-DRM-plane
// pitch/offset and which V4L2 dmabuf fd each imports from.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <linux/videodev2.h>
#include <system_error>

namespace drm::scene::detail {

// AddFB2 takes at most 4 planes.
inline constexpr std::size_t k_drm_max_planes = 4;

// Ceilings on V4L2-echoed image dims and per-row byte counts. The V4L2 ABI
// exposes plain u32 dims with no upper bound; a quirky or hostile capture
// device can echo absurd values that (a) wrap `bytesperline * height` in u32 and
// (b) point AddFB2 offsets at attacker-controlled byte ranges of the same
// buffer. 16K is past every shipping display; 64K is past any real stride.
inline constexpr std::uint32_t k_max_image_dim = 16384U;
inline constexpr std::uint32_t k_max_bytes_per_line = 65536U;

// How a V4L2 CAPTURE format echo maps onto DRM AddFB2's per-plane
// handle/pitch/offset arrays. Each DRM plane records which V4L2 dmabuf_fd it
// imports from (`v4l2_plane_idx`) and where in that fd the plane starts.
struct DrmPlaneLayout {
  std::uint32_t num_drm_planes{0};
  std::array<std::uint32_t, k_drm_max_planes> pitch{};
  std::array<std::uint32_t, k_drm_max_planes> offset{};
  std::array<std::uint8_t, k_drm_max_planes> v4l2_plane_idx{};
};

// Derive the DRM plane layout for `drm_fourcc` from a V4L2 CAPTURE format echo.
// Handles three shapes:
//   * single-V4L2-plane semi-planar 4:2:0 (NV12 / P010 / P012 / P016) -> 2 DRM
//     planes (Y, interleaved UV) via offset math;
//   * single-V4L2-plane planar 4:2:0 (YUV420 / YVU420) -> 3 DRM planes (Y and
//     two quarter-resolution chroma planes);
//   * everything else -> 1 DRM plane per V4L2 plane (MPLANE 1:1, packed YUYV,
//     RGB, ...).
// Returns an error_code on malformed or hostile dims (zero/oversized height,
// stride, plane count, or an offset that would truncate AddFB2's u32 field).
[[nodiscard]] std::error_code derive_drm_plane_layout(const v4l2_format& cap_fmt, bool is_mplane,
                                                      std::uint32_t drm_fourcc,
                                                      DrmPlaneLayout& out) noexcept;

}  // namespace drm::scene::detail
