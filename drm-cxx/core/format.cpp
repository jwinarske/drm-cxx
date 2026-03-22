// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "format.hpp"

#include <drm_fourcc.h>

namespace drm {

std::string_view format_name(uint32_t format) {
  switch (format) {
  case DRM_FORMAT_C8:          return "C8";
  case DRM_FORMAT_RGB332:      return "RGB332";
  case DRM_FORMAT_BGR233:      return "BGR233";
  case DRM_FORMAT_XRGB4444:    return "XRGB4444";
  case DRM_FORMAT_XBGR4444:    return "XBGR4444";
  case DRM_FORMAT_RGBX4444:    return "RGBX4444";
  case DRM_FORMAT_BGRX4444:    return "BGRX4444";
  case DRM_FORMAT_ARGB4444:    return "ARGB4444";
  case DRM_FORMAT_ABGR4444:    return "ABGR4444";
  case DRM_FORMAT_RGBA4444:    return "RGBA4444";
  case DRM_FORMAT_BGRA4444:    return "BGRA4444";
  case DRM_FORMAT_XRGB1555:    return "XRGB1555";
  case DRM_FORMAT_XBGR1555:    return "XBGR1555";
  case DRM_FORMAT_RGBX5551:    return "RGBX5551";
  case DRM_FORMAT_BGRX5551:    return "BGRX5551";
  case DRM_FORMAT_ARGB1555:    return "ARGB1555";
  case DRM_FORMAT_ABGR1555:    return "ABGR1555";
  case DRM_FORMAT_RGBA5551:    return "RGBA5551";
  case DRM_FORMAT_BGRA5551:    return "BGRA5551";
  case DRM_FORMAT_RGB565:      return "RGB565";
  case DRM_FORMAT_BGR565:      return "BGR565";
  case DRM_FORMAT_RGB888:      return "RGB888";
  case DRM_FORMAT_BGR888:      return "BGR888";
  case DRM_FORMAT_XRGB8888:    return "XRGB8888";
  case DRM_FORMAT_XBGR8888:    return "XBGR8888";
  case DRM_FORMAT_RGBX8888:    return "RGBX8888";
  case DRM_FORMAT_BGRX8888:    return "BGRX8888";
  case DRM_FORMAT_ARGB8888:    return "ARGB8888";
  case DRM_FORMAT_ABGR8888:    return "ABGR8888";
  case DRM_FORMAT_RGBA8888:    return "RGBA8888";
  case DRM_FORMAT_BGRA8888:    return "BGRA8888";
  case DRM_FORMAT_XRGB2101010: return "XRGB2101010";
  case DRM_FORMAT_XBGR2101010: return "XBGR2101010";
  case DRM_FORMAT_RGBX1010102: return "RGBX1010102";
  case DRM_FORMAT_BGRX1010102: return "BGRX1010102";
  case DRM_FORMAT_ARGB2101010: return "ARGB2101010";
  case DRM_FORMAT_ABGR2101010: return "ABGR2101010";
  case DRM_FORMAT_RGBA1010102: return "RGBA1010102";
  case DRM_FORMAT_BGRA1010102: return "BGRA1010102";
  case DRM_FORMAT_XRGB16161616F: return "XRGB16161616F";
  case DRM_FORMAT_XBGR16161616F: return "XBGR16161616F";
  case DRM_FORMAT_ARGB16161616F: return "ARGB16161616F";
  case DRM_FORMAT_ABGR16161616F: return "ABGR16161616F";
  case DRM_FORMAT_NV12:        return "NV12";
  case DRM_FORMAT_NV21:        return "NV21";
  case DRM_FORMAT_NV16:        return "NV16";
  case DRM_FORMAT_NV61:        return "NV61";
  case DRM_FORMAT_YUV410:      return "YUV410";
  case DRM_FORMAT_YVU410:      return "YVU410";
  case DRM_FORMAT_YUV411:      return "YUV411";
  case DRM_FORMAT_YVU411:      return "YVU411";
  case DRM_FORMAT_YUV420:      return "YUV420";
  case DRM_FORMAT_YVU420:      return "YVU420";
  case DRM_FORMAT_YUV422:      return "YUV422";
  case DRM_FORMAT_YVU422:      return "YVU422";
  case DRM_FORMAT_YUV444:      return "YUV444";
  case DRM_FORMAT_YVU444:      return "YVU444";
  default:                     return "unknown";
  }
}

uint32_t format_bpp(uint32_t format) {
  switch (format) {
  case DRM_FORMAT_C8:
  case DRM_FORMAT_RGB332:
  case DRM_FORMAT_BGR233:
    return 8;
  case DRM_FORMAT_XRGB4444:
  case DRM_FORMAT_XBGR4444:
  case DRM_FORMAT_RGBX4444:
  case DRM_FORMAT_BGRX4444:
  case DRM_FORMAT_ARGB4444:
  case DRM_FORMAT_ABGR4444:
  case DRM_FORMAT_RGBA4444:
  case DRM_FORMAT_BGRA4444:
  case DRM_FORMAT_XRGB1555:
  case DRM_FORMAT_XBGR1555:
  case DRM_FORMAT_RGBX5551:
  case DRM_FORMAT_BGRX5551:
  case DRM_FORMAT_ARGB1555:
  case DRM_FORMAT_ABGR1555:
  case DRM_FORMAT_RGBA5551:
  case DRM_FORMAT_BGRA5551:
  case DRM_FORMAT_RGB565:
  case DRM_FORMAT_BGR565:
    return 16;
  case DRM_FORMAT_RGB888:
  case DRM_FORMAT_BGR888:
    return 24;
  case DRM_FORMAT_XRGB8888:
  case DRM_FORMAT_XBGR8888:
  case DRM_FORMAT_RGBX8888:
  case DRM_FORMAT_BGRX8888:
  case DRM_FORMAT_ARGB8888:
  case DRM_FORMAT_ABGR8888:
  case DRM_FORMAT_RGBA8888:
  case DRM_FORMAT_BGRA8888:
  case DRM_FORMAT_XRGB2101010:
  case DRM_FORMAT_XBGR2101010:
  case DRM_FORMAT_RGBX1010102:
  case DRM_FORMAT_BGRX1010102:
  case DRM_FORMAT_ARGB2101010:
  case DRM_FORMAT_ABGR2101010:
  case DRM_FORMAT_RGBA1010102:
  case DRM_FORMAT_BGRA1010102:
    return 32;
  case DRM_FORMAT_XRGB16161616F:
  case DRM_FORMAT_XBGR16161616F:
  case DRM_FORMAT_ARGB16161616F:
  case DRM_FORMAT_ABGR16161616F:
    return 64;
  default:
    return 0;
  }
}

} // namespace drm
