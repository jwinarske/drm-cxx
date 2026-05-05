// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/animator.hpp — per-window focus/hover animation state.
//
// The renderer's visual transitions (focused ⇄ blurred shadow + rim,
// fill ⇄ hover button) are continuous, but the shell only ever sees
// discrete events (focus changed, pointer moved over button N). The
// `WindowAnim` here bridges the two: the shell calls retarget_*() when
// state changes, then `tick(dt, duration)` once per frame, then
// apply_to() to mirror the resulting progress fields into the
// renderer's WindowState.
//
// Easing is ease-out-cubic — cheap, settles convincingly, and matches
// the standard window-focus feel without per-axis tuning. Duration is
// passed in by the caller (typically `theme.animation_duration_ms`)
// so a theme swap doesn't change the in-flight tween rate mid-frame.
//
// Header is Blend2D-free and standalone — only depends on
// window_state.hpp + <chrono>. Animator state is value-type per
// window; there is no global registry.

#pragma once

#include "window_state.hpp"

#include <chrono>

namespace drm::csd {

class WindowAnim {
 public:
  // Construct sitting at `focused`'s steady state (progress 1 if
  // focused, 0 if blurred). New documents are created at their target
  // visuals so the spawn frame doesn't fade in from blurred.
  explicit WindowAnim(bool focused = false) noexcept;

  // Set the focus target. No-op when already targeting that value.
  // Captures the current progress as the start of the new tween so
  // mid-flight retargets ease cleanly without snapping.
  void retarget_focus(bool focused) noexcept;

  // Set the hover target. None starts a fade-out of the currently-
  // painted button (kept on screen until progress reaches 0); a
  // non-None value snaps the painted slot to the new button and
  // animates it from 0 to 1. Mirrors GTK / GNOME behavior — sweeping
  // the cursor across buttons skips the fade-out so the active button
  // is always the one under the pointer.
  void retarget_hover(HoverButton hover) noexcept;

  // Snap both timelines to their targets immediately. Used on theme
  // reload (no point animating between identical visuals) and on
  // explicit "no animation" overrides. Leaves targets unchanged.
  void snap() noexcept;

  // Advance both timelines by `dt`. `duration` is the per-edge tween
  // length (typically the theme's animation_duration_ms); zero or
  // negative durations short-circuit to snap(). Returns true while at
  // least one timeline is still in flight, so the shell can keep its
  // frame_dirty marker set across the tween.
  bool tick(std::chrono::milliseconds dt, std::chrono::milliseconds duration) noexcept;

  // True iff at least one timeline still has progress to apply.
  [[nodiscard]] bool active() const noexcept;

  // Mirror the current animator state into `state`. Writes
  // focus_progress, hover_progress, and `state.hover` (the painted
  // button, which can lag retarget_hover(None) until the fade-out
  // completes). Leaves title / focused / dirty alone — those are
  // shell-owned inputs, not animator outputs.
  void apply_to(WindowState& state) const noexcept;

  // ── Diagnostics ──────────────────────────────────────────────────

  [[nodiscard]] float focus_progress() const noexcept { return focus_progress_; }
  [[nodiscard]] float hover_progress() const noexcept { return hover_progress_; }
  [[nodiscard]] HoverButton hover_painted() const noexcept { return hover_painted_; }

 private:
  // Focus timeline.
  bool focus_target_{false};
  float focus_start_{0.0F};
  float focus_progress_{0.0F};
  std::chrono::milliseconds focus_elapsed_{0};
  bool focus_running_{false};

  // Hover timeline. `hover_painted_` is the button the renderer should
  // lerp; `hover_target_` is the button the shell most recently
  // requested. They diverge during a fade-out (target = None, painted
  // = the still-fading-out button).
  HoverButton hover_target_{HoverButton::None};
  HoverButton hover_painted_{HoverButton::None};
  float hover_start_{0.0F};
  float hover_progress_{0.0F};
  std::chrono::milliseconds hover_elapsed_{0};
  bool hover_running_{false};
};

// Cubic ease-out: 1 - (1 - t)^3, clamped to [0, 1]. Public so tests
// and the renderer can sample identical curves.
[[nodiscard]] float ease_out_cubic(float t) noexcept;

}  // namespace drm::csd