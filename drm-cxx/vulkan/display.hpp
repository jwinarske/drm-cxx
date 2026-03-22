// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <system_error>

namespace drm::vulkan {

class Display {
public:
  static std::expected<Display, std::error_code> create();

  ~Display();
  Display(Display&&) noexcept;
  Display& operator=(Display&&) noexcept;
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

private:
  Display() = default;
};

} // namespace drm::vulkan
