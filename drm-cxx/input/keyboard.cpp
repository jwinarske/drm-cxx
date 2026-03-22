// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "keyboard.hpp"

namespace drm::input {

Keyboard::~Keyboard() = default;
Keyboard::Keyboard(Keyboard&&) noexcept = default;
Keyboard& Keyboard::operator=(Keyboard&&) noexcept = default;

std::expected<Keyboard, std::error_code>
Keyboard::create([[maybe_unused]] std::string_view keymap_path) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace drm::input
