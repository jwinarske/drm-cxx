// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "format.hpp"

namespace drm {

std::string_view format_name([[maybe_unused]] uint32_t format) {
  return "unknown";
}

uint32_t format_bpp([[maybe_unused]] uint32_t format) {
  return 0;
}

} // namespace drm
