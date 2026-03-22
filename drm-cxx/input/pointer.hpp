// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <unordered_set>

namespace drm::input {

// Pointer state tracker — accumulates motion deltas and button state.
class Pointer {
 public:
  void accumulate_motion(double dx, double dy) noexcept;
  void set_button(uint32_t button, bool pressed);

  [[nodiscard]] double x() const noexcept;
  [[nodiscard]] double y() const noexcept;
  [[nodiscard]] bool button_pressed(uint32_t button) const noexcept;

  void reset_position(double x, double y) noexcept;

 private:
  double x_{};
  double y_{};
  std::unordered_set<uint32_t> buttons_;
};

}  // namespace drm::input
