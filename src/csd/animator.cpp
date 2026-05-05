// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "animator.hpp"

#include "window_state.hpp"

#include <algorithm>
#include <chrono>

namespace drm::csd {

namespace {

float clamp01(float v) noexcept {
  return std::clamp(v, 0.0F, 1.0F);
}

float lerp(float a, float b, float t) noexcept {
  return a + ((b - a) * t);
}

}  // namespace

float ease_out_cubic(float t) noexcept {
  const float c = clamp01(t);
  const float inv = 1.0F - c;
  return 1.0F - (inv * inv * inv);
}

WindowAnim::WindowAnim(bool focused) noexcept
    : focus_target_(focused), focus_progress_(focused ? 1.0F : 0.0F) {}

void WindowAnim::retarget_focus(bool focused) noexcept {
  if (focus_target_ == focused) {
    return;
  }
  focus_target_ = focused;
  focus_start_ = focus_progress_;
  focus_elapsed_ = std::chrono::milliseconds{0};
  focus_running_ = true;
}

void WindowAnim::retarget_hover(HoverButton hover) noexcept {
  if (hover == hover_target_) {
    return;
  }
  hover_target_ = hover;
  if (hover != HoverButton::None) {
    hover_painted_ = hover;
    hover_start_ = 0.0F;
    hover_progress_ = 0.0F;
  } else {
    hover_start_ = hover_progress_;
  }
  hover_elapsed_ = std::chrono::milliseconds{0};
  hover_running_ = true;
}

void WindowAnim::snap() noexcept {
  focus_progress_ = focus_target_ ? 1.0F : 0.0F;
  focus_start_ = focus_progress_;
  focus_elapsed_ = std::chrono::milliseconds{0};
  focus_running_ = false;

  hover_painted_ = hover_target_;
  hover_progress_ = hover_target_ == HoverButton::None ? 0.0F : 1.0F;
  hover_start_ = hover_progress_;
  hover_elapsed_ = std::chrono::milliseconds{0};
  hover_running_ = false;
}

bool WindowAnim::tick(std::chrono::milliseconds dt, std::chrono::milliseconds duration) noexcept {
  if (duration <= std::chrono::milliseconds{0}) {
    snap();
    return false;
  }

  if (focus_running_) {
    focus_elapsed_ += dt;
    const float t =
        clamp01(static_cast<float>(focus_elapsed_.count()) / static_cast<float>(duration.count()));
    const float target = focus_target_ ? 1.0F : 0.0F;
    focus_progress_ = lerp(focus_start_, target, ease_out_cubic(t));
    if (t >= 1.0F) {
      focus_progress_ = target;
      focus_running_ = false;
    }
  }

  if (hover_running_) {
    hover_elapsed_ += dt;
    const float t =
        clamp01(static_cast<float>(hover_elapsed_.count()) / static_cast<float>(duration.count()));
    // Target progress for the painted button: 1 when the painted
    // button is still the requested one, 0 when we're fading it out
    // (target == None, painted retained until the fade completes).
    const float target =
        (hover_target_ != HoverButton::None && hover_target_ == hover_painted_) ? 1.0F : 0.0F;
    hover_progress_ = lerp(hover_start_, target, ease_out_cubic(t));
    if (t >= 1.0F) {
      hover_progress_ = target;
      hover_running_ = false;
      if (target == 0.0F) {
        hover_painted_ = HoverButton::None;
      }
    }
  }

  return active();
}

bool WindowAnim::active() const noexcept {
  return focus_running_ || hover_running_;
}

void WindowAnim::apply_to(WindowState& state) const noexcept {
  state.focus_progress = focus_progress_;
  state.hover = hover_painted_;
  state.hover_progress = hover_progress_;
}

}  // namespace drm::csd