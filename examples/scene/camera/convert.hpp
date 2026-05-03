// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// CPU-side libyuv wrappers used by the camera example to deliver an
// ARGB8888 (== DRM_FORMAT_XRGB8888 byte-for-byte; the alpha lane is
// ignored at scanout) destination buffer for any camera fourcc the
// scanout plane can't display directly. Converting to RGB on the CPU
// sidesteps the YCbCr-matrix-on-the-plane question entirely — the
// display engine just blits RGB pixels — and libyuv applies the
// correct BT.601 limited matrix internally for the libcamera UVC
// pipeline's colorspace metadata.
//
// libyuv has hand-tuned SIMD paths for AVX2/SSSE3 (x86_64) and NEON
// (aarch64) and dispatches at runtime; the MJPEG path further
// delegates the entropy decode to libjpeg-turbo's SIMD decoder.

#pragma once

#include <cstddef>
#include <cstdint>

namespace drm::examples::camera {

// libyuv `YUY2ToARGB`: applies BT.601 limited-range YCbCr → sRGB on
// the way to ARGB. Returns false on libyuv error.
[[nodiscard]] bool yuy2_to_xrgb(const std::uint8_t* src, std::uint32_t src_stride,
                                std::uint8_t* dst, std::uint32_t dst_pitch, std::uint32_t width,
                                std::uint32_t height) noexcept;

// libyuv `NV12ToARGB`: same matrix as yuy2_to_xrgb. Used as the
// runtime fallback when zero-copy NV12 PRIME-import fails (e.g. UVC's
// vmalloc-backed dma-bufs aren't scanout-capable on amdgpu DC).
[[nodiscard]] bool nv12_to_xrgb(const std::uint8_t* src_y, const std::uint8_t* src_uv,
                                std::uint32_t src_pitch, std::uint8_t* dst, std::uint32_t dst_pitch,
                                std::uint32_t width, std::uint32_t height) noexcept;

// libyuv `MJPGToARGB`: JPEG entropy decode (libjpeg-turbo) plus
// YCbCr → sRGB color conversion in one pass.
[[nodiscard]] bool mjpeg_to_xrgb(const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst,
                                 std::uint32_t dst_pitch, std::uint32_t width,
                                 std::uint32_t height) noexcept;

}  // namespace drm::examples::camera
