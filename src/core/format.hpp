// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>

namespace drm {

/// Name of a DRM fourcc, exactly the `DRM_FORMAT_*` macro suffix ("XRGB8888"
/// for `DRM_FORMAT_XRGB8888`). Returns "unknown" -- never an empty view -- for
/// any code not listed, so a caller can log the result unconditionally.
///
/// The view points at a string literal, so it outlives every caller and is
/// safe to store. Coverage is the formats drm-cxx handles rather than all of
/// drm_fourcc.h; adding one means adding a case.
[[nodiscard]] std::string_view format_name(uint32_t format);
[[nodiscard]] uint32_t format_bpp(uint32_t format);

}  // namespace drm
