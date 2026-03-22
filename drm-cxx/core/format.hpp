// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>

namespace drm {

[[nodiscard]] std::string_view format_name(uint32_t format);
[[nodiscard]] uint32_t format_bpp(uint32_t format);

}  // namespace drm
