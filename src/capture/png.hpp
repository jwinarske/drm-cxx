// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// capture/png.hpp — encode a drm::capture::Image as a PNG file.
//
// Thin wrapper around Blend2D's BLImageCodec("PNG"). The encoder lives
// inside Blend2D, so this TU is the only thing that links against it
// on behalf of the PNG path — callers just pass an Image + path.
//
// Format: 8-bit-per-channel RGBA PNG. Alpha is written straight
// (un-premultiplied) so the on-disk PNG round-trips correctly through
// tools that expect straight alpha. Conversion from Image's
// premultiplied storage to straight alpha happens inside this TU.

#pragma once

#include "capture/snapshot.hpp"
#include "detail/expected.hpp"

#include <string_view>
#include <system_error>

namespace drm::capture {

/// Encode `image` as a PNG at `path`. The path is created / truncated
/// via Blend2D's file API; the directory must already exist.
///
/// Returns an error when:
///   - the image is empty;
///   - the PNG codec is not registered in the linked Blend2D (should
///     never happen with a standard build);
///   - file I/O fails.
[[nodiscard]] drm::expected<void, std::error_code> write_png(const Image& image,
                                                             std::string_view path);

}  // namespace drm::capture