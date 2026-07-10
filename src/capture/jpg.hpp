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
// Format: baseline (non-progressive) JCS_RGB JPEG, 8-bit. JPEG has no alpha
// channel, so the Image's premultiplied ARGB8888 pixels are un-premultiplied
// to straight RGB during encode (an opaque screenshot round-trips exactly;
// only genuinely translucent source pixels lose their coverage, as they must
// for an opaque format). `quality` is the standard libjpeg 1..100 scale.

#pragma once

#include "capture/snapshot.hpp"
#include "detail/expected.hpp"

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

}  // namespace drm::capture
