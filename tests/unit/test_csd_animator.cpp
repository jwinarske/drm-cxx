// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::csd::WindowAnim and ease_out_cubic. Pure CPU
// tests — the animator owns no resources and depends only on the
// public WindowState header.

#include <drm-cxx/csd/animator.hpp>
#include <drm-cxx/csd/window_state.hpp>

#include <chrono>
#include <gtest/gtest.h>

using drm::csd::ease_out_cubic;
using drm::csd::HoverButton;
using drm::csd::WindowAnim;
using drm::csd::WindowState;
using std::chrono::milliseconds;

namespace {

constexpr float k_eps = 1e-4F;

}  // namespace

// ── ease_out_cubic ──────────────────────────────────────────────

TEST(EaseOutCubic, EndpointsExact) {
  EXPECT_NEAR(ease_out_cubic(0.0F), 0.0F, k_eps);
  EXPECT_NEAR(ease_out_cubic(1.0F), 1.0F, k_eps);
}

TEST(EaseOutCubic, ClampsBelowZero) {
  EXPECT_NEAR(ease_out_cubic(-0.5F), 0.0F, k_eps);
}

TEST(EaseOutCubic, ClampsAboveOne) {
  EXPECT_NEAR(ease_out_cubic(1.5F), 1.0F, k_eps);
}

TEST(EaseOutCubic, MonotonicIncreasing) {
  float prev = ease_out_cubic(0.0F);
  for (int i = 1; i <= 100; ++i) {
    const float t = static_cast<float>(i) / 100.0F;
    const float v = ease_out_cubic(t);
    EXPECT_GE(v, prev) << "regression at t=" << t;
    prev = v;
  }
}

TEST(EaseOutCubic, FastStart) {
  // ease-out should cover more ground in the first half than linear
  // would; expect 0.5 to map well above 0.5.
  EXPECT_GT(ease_out_cubic(0.5F), 0.7F);
}

// ── WindowAnim::retarget_focus ─────────────────────────────────

TEST(WindowAnimFocus, ConstructsAtTargetSteadyState) {
  const WindowAnim a(/*focused=*/true);
  EXPECT_NEAR(a.focus_progress(), 1.0F, k_eps);
  EXPECT_FALSE(a.active());

  const WindowAnim b(/*focused=*/false);
  EXPECT_NEAR(b.focus_progress(), 0.0F, k_eps);
  EXPECT_FALSE(b.active());
}

TEST(WindowAnimFocus, RetargetSameNoOp) {
  WindowAnim a(/*focused=*/true);
  a.retarget_focus(true);
  EXPECT_FALSE(a.active());
}

TEST(WindowAnimFocus, RetargetStartsTimeline) {
  WindowAnim a(/*focused=*/false);
  a.retarget_focus(true);
  EXPECT_TRUE(a.active());
  EXPECT_NEAR(a.focus_progress(), 0.0F, k_eps);
}

TEST(WindowAnimFocus, ReachesTargetAfterFullDuration) {
  WindowAnim a(/*focused=*/false);
  a.retarget_focus(true);
  // Tick exactly the duration in one step.
  EXPECT_FALSE(a.tick(milliseconds(180), milliseconds(180)));  // full duration
  EXPECT_NEAR(a.focus_progress(), 1.0F, k_eps);
  EXPECT_FALSE(a.active());
}

TEST(WindowAnimFocus, OvershootDtClampsToTarget) {
  WindowAnim a(/*focused=*/false);
  a.retarget_focus(true);
  EXPECT_FALSE(a.tick(milliseconds(500), milliseconds(180)));
  EXPECT_NEAR(a.focus_progress(), 1.0F, k_eps);
}

TEST(WindowAnimFocus, MidFlightProgressMonotonic) {
  WindowAnim a(/*focused=*/false);
  a.retarget_focus(true);
  float prev = a.focus_progress();
  for (int i = 0; i < 18; ++i) {
    a.tick(milliseconds(10), milliseconds(180));
    EXPECT_GE(a.focus_progress(), prev);
    prev = a.focus_progress();
  }
  EXPECT_NEAR(a.focus_progress(), 1.0F, k_eps);
}

TEST(WindowAnimFocus, MidFlightRetargetEasesFromCurrent) {
  WindowAnim a(/*focused=*/false);
  a.retarget_focus(true);
  a.tick(milliseconds(60), milliseconds(180));  // ~mid-tween
  const float mid = a.focus_progress();
  ASSERT_GT(mid, 0.0F);
  ASSERT_LT(mid, 1.0F);
  // Reverse target — the new tween should start from `mid` and end at 0.
  a.retarget_focus(false);
  EXPECT_TRUE(a.active());
  EXPECT_NEAR(a.focus_progress(), mid, k_eps);
  a.tick(milliseconds(180), milliseconds(180));
  EXPECT_NEAR(a.focus_progress(), 0.0F, k_eps);
  EXPECT_FALSE(a.active());
}

// ── WindowAnim::retarget_hover ────────────────────────────────

TEST(WindowAnimHover, NoneByDefault) {
  const WindowAnim a;
  EXPECT_EQ(a.hover_painted(), HoverButton::None);
  EXPECT_NEAR(a.hover_progress(), 0.0F, k_eps);
}

TEST(WindowAnimHover, EnterStartsFromZero) {
  WindowAnim a;
  a.retarget_hover(HoverButton::Close);
  EXPECT_TRUE(a.active());
  EXPECT_EQ(a.hover_painted(), HoverButton::Close);
  EXPECT_NEAR(a.hover_progress(), 0.0F, k_eps);
}

TEST(WindowAnimHover, EnterCompletesAtOne) {
  WindowAnim a;
  a.retarget_hover(HoverButton::Close);
  a.tick(milliseconds(180), milliseconds(180));
  EXPECT_NEAR(a.hover_progress(), 1.0F, k_eps);
  EXPECT_EQ(a.hover_painted(), HoverButton::Close);
  EXPECT_FALSE(a.active());
}

TEST(WindowAnimHover, LeaveFadesPaintedToZero) {
  WindowAnim a;
  a.retarget_hover(HoverButton::Close);
  a.tick(milliseconds(180), milliseconds(180));
  EXPECT_NEAR(a.hover_progress(), 1.0F, k_eps);

  a.retarget_hover(HoverButton::None);
  EXPECT_TRUE(a.active());
  // Still painting Close until the fade-out completes.
  EXPECT_EQ(a.hover_painted(), HoverButton::Close);
  a.tick(milliseconds(180), milliseconds(180));
  EXPECT_NEAR(a.hover_progress(), 0.0F, k_eps);
  EXPECT_EQ(a.hover_painted(), HoverButton::None);
}

TEST(WindowAnimHover, SwitchSnapsToNewButton) {
  WindowAnim a;
  a.retarget_hover(HoverButton::Close);
  a.tick(milliseconds(60), milliseconds(180));
  ASSERT_EQ(a.hover_painted(), HoverButton::Close);

  // Sweeping across to Minimize snaps the painted button without
  // waiting for the fade-out — matches GTK / GNOME's behavior.
  a.retarget_hover(HoverButton::Minimize);
  EXPECT_EQ(a.hover_painted(), HoverButton::Minimize);
  EXPECT_NEAR(a.hover_progress(), 0.0F, k_eps);
}

// ── snap ──────────────────────────────────────────────────────

TEST(WindowAnimSnap, JumpsToTargets) {
  WindowAnim a(/*focused=*/false);
  a.retarget_focus(true);
  a.retarget_hover(HoverButton::Maximize);
  a.snap();
  EXPECT_FALSE(a.active());
  EXPECT_NEAR(a.focus_progress(), 1.0F, k_eps);
  EXPECT_EQ(a.hover_painted(), HoverButton::Maximize);
  EXPECT_NEAR(a.hover_progress(), 1.0F, k_eps);
}

TEST(WindowAnimTick, ZeroDurationSnaps) {
  WindowAnim a(/*focused=*/false);
  a.retarget_focus(true);
  EXPECT_FALSE(a.tick(milliseconds(16), milliseconds(0)));
  EXPECT_NEAR(a.focus_progress(), 1.0F, k_eps);
  EXPECT_FALSE(a.active());
}

// ── apply_to ──────────────────────────────────────────────────

TEST(WindowAnimApply, MirrorsProgressIntoState) {
  WindowAnim a(/*focused=*/true);
  a.retarget_hover(HoverButton::Close);
  a.tick(milliseconds(60), milliseconds(180));

  WindowState s;
  s.title = "preserved";
  s.focused = true;
  s.dirty = drm::csd::k_dirty_focus;
  a.apply_to(s);

  EXPECT_EQ(s.title, "preserved");  // animator never touches title.
  EXPECT_TRUE(s.focused);           // animator never touches binary focused.
  EXPECT_EQ(s.dirty, drm::csd::k_dirty_focus);
  EXPECT_EQ(s.hover, HoverButton::Close);
  EXPECT_GE(s.focus_progress, 0.0F);
  EXPECT_LE(s.focus_progress, 1.0F);
  EXPECT_GE(s.hover_progress, 0.0F);
  EXPECT_LE(s.hover_progress, 1.0F);
}