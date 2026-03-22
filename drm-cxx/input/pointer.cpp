// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "pointer.hpp"

#include <cstdint>

namespace drm::input {

void Pointer::accumulate_motion(double dx, double dy) noexcept {
  x_ += dx;
  y_ += dy;
}

void Pointer::set_button(uint32_t button, bool pressed) noexcept {
  // Use lower bits for common buttons (BTN_LEFT=0x110, etc.)
  uint32_t const bit = button & 0x1F;
  if (pressed) {
    buttons_ |= (1U << bit);
  } else {
    buttons_ &= ~(1U << bit);
  }
}

double Pointer::x() const noexcept {
  return x_;
}
double Pointer::y() const noexcept {
  return y_;
}

bool Pointer::button_pressed(uint32_t button) const noexcept {
  uint32_t const bit = button & 0x1F;
  return (buttons_ & (1U << bit)) != 0;
}

void Pointer::reset_position(double x, double y) noexcept {
  x_ = x;
  y_ = y;
}

}  // namespace drm::input
