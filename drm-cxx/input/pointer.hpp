// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace drm::input {

// Pointer state tracker — accumulates motion deltas and button state.
class Pointer {
 public:
  void accumulate_motion(double dx, double dy) noexcept;
  void set_button(uint32_t button, bool pressed) noexcept;

  [[nodiscard]] double x() const noexcept;
  [[nodiscard]] double y() const noexcept;
  [[nodiscard]] bool button_pressed(uint32_t button) const noexcept;

  void reset_position(double x, double y) noexcept;

 private:
  double x_{};
  double y_{};
  uint32_t buttons_{};  // bitmask of pressed buttons
};

}  // namespace drm::input
