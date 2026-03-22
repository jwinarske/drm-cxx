// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "pointer.hpp"

#include <cstdint>

namespace drm::input {

void Pointer::accumulate_motion(double dx, double dy) noexcept {
  x_ += dx;
  y_ += dy;
}

void Pointer::set_button(uint32_t button, bool pressed) {
  if (pressed) {
    buttons_.insert(button);
  } else {
    buttons_.erase(button);
  }
}

double Pointer::x() const noexcept {
  return x_;
}
double Pointer::y() const noexcept {
  return y_;
}

bool Pointer::button_pressed(uint32_t button) const noexcept {
  return buttons_.contains(button);
}

void Pointer::reset_position(double x, double y) noexcept {
  x_ = x;
  y_ = y;
}

}  // namespace drm::input
