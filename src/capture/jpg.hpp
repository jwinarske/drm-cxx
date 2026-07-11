// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// capture/jpg.hpp — encode a drm::capture::Image as a baseline JPEG file.
//
// The PNG path (capture/png.hpp) rides on Blend2D's built-in codec, but
// Blend2D ships no JPEG *encoder* (decode only), so the JPEG path links a
// separate dependency: libjpeg (satisfied by libjpeg-turbo on every modern
// distro). That dependency is optional and gated behind its own build
// switch — CMake `DRM_CXX_CAPTURE_JPG`, Meson `capture-jpg` — which defines
// `DRM_CXX_HAS_LIBJPEG`. This TU (and its install) only exist when both
// Blend2D (for the Image type) and libjpeg are available; callers that want
// to branch at compile time can test `DRM_CXX_HAS_LIBJPEG`.
//
// Format: baseline (non-progressive) JPEG, 8-bit. `write_jpg` takes a
// premultiplied ARGB8888 Image and encodes JCS_RGB (JPEG has no alpha, so the
// pixels are un-premultiplied to straight RGB — an opaque screenshot
// round-trips exactly; only genuinely translucent pixels lose their coverage).
// `write_jpg_nv12` takes an NV12 (YCbCr 4:2:0) frame straight from a camera or
// decoder and encodes it in JPEG's native color space with no RGB round-trip.
// `quality` is the standard libjpeg 1..100 scale.

#pragma once

#include "capture/snapshot.hpp"
#include "detail/expected.hpp"

#include <cstdint>
#include <string_view>
#include <system_error>

namespace drm::capture {

/// Encode `image` as a baseline JPEG at `path` with the given `quality`
/// (1..100, clamped; 90 is a good default for screenshots). The file is
/// created / truncated; the directory must already exist.
///
/// Returns an error when:
///   - the image is empty (`errc::invalid_argument`);
///   - the file cannot be opened for writing (the open errno);
///   - libjpeg reports a fatal compression error (`errc::io_error`).
[[nodiscard]] drm::expected<void, std::error_code> write_jpg(const Image& image,
                                                             std::string_view path,
                                                             int quality = 90);

/// Encode an NV12 (YCbCr 4:2:0 semi-planar) frame as a baseline JPEG at `path`.
/// `y` is the luma plane (`width` x `height`, row pitch `y_stride`); `uv` is
/// the interleaved chroma plane (Cb,Cr byte pairs; `width` bytes x `height/2`
/// rows, row pitch `uv_stride`). NV12 already *is* JPEG's native color space,
/// so no color conversion happens — the de-interleaved planes feed libjpeg's
/// raw 4:2:0 data path directly. A frame whose height/width isn't a multiple of
/// the 16x16 MCU is edge-padded internally and the output is cropped back to
/// `width` x `height`.
///
/// Returns an error when:
///   - `y`/`uv` is null or a dimension is zero (`errc::invalid_argument`);
///   - the file cannot be opened for writing (the open errno);
///   - libjpeg reports a fatal compression error (`errc::io_error`).
[[nodiscard]] drm::expected<void, std::error_code> write_jpg_nv12(
    const std::uint8_t* y, const std::uint8_t* uv, std::uint32_t y_stride, std::uint32_t uv_stride,
    std::uint32_t width, std::uint32_t height, std::string_view path, int quality = 90);

}  // namespace drm::capture
