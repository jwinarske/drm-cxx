// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <system_error>

namespace drm::input {

class Keyboard {
public:
  static std::expected<Keyboard, std::error_code>
    create(std::string_view keymap_path = {});

  ~Keyboard();
  Keyboard(Keyboard&&) noexcept;
  Keyboard& operator=(Keyboard&&) noexcept;
  Keyboard(const Keyboard&) = delete;
  Keyboard& operator=(const Keyboard&) = delete;

private:
  Keyboard() = default;
};

} // namespace drm::input
