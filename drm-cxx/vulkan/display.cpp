// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "display.hpp"

namespace drm::vulkan {

Display::~Display() = default;
Display::Display(Display&&) noexcept = default;
Display& Display::operator=(Display&&) noexcept = default;

std::expected<Display, std::error_code> Display::create() {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace drm::vulkan
