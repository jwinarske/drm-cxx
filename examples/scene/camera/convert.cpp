// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "convert.hpp"

#include <cstddef>
#include <cstdint>
#include <libyuv/convert_argb.h>

namespace drm::examples::camera {

bool yuy2_to_xrgb(const std::uint8_t* src, std::uint32_t src_stride, std::uint8_t* dst,
                  std::uint32_t dst_pitch, std::uint32_t width, std::uint32_t height) noexcept {
  return libyuv::YUY2ToARGB(src, static_cast<int>(src_stride), dst, static_cast<int>(dst_pitch),
                            static_cast<int>(width), static_cast<int>(height)) == 0;
}

bool nv12_to_xrgb(const std::uint8_t* src_y, const std::uint8_t* src_uv, std::uint32_t src_pitch,
                  std::uint8_t* dst, std::uint32_t dst_pitch, std::uint32_t width,
                  std::uint32_t height) noexcept {
  return libyuv::NV12ToARGB(src_y, static_cast<int>(src_pitch), src_uv, static_cast<int>(src_pitch),
                            dst, static_cast<int>(dst_pitch), static_cast<int>(width),
                            static_cast<int>(height)) == 0;
}

bool mjpeg_to_xrgb(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                   std::uint32_t dst_pitch, std::uint32_t width, std::uint32_t height) noexcept {
  return libyuv::MJPGToARGB(src, src_size, dst, static_cast<int>(dst_pitch),
                            static_cast<int>(width), static_cast<int>(height),
                            static_cast<int>(width), static_cast<int>(height)) == 0;
}

}  // namespace drm::examples::camera
