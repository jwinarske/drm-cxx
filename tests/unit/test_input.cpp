// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "input/event_dispatcher.hpp"
#include "input/keyboard.hpp"
#include "input/pointer.hpp"
#include "input/seat.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <variant>

// ── Event type tests ──────────────────────────────────────────

TEST(InputEventTest, KeyboardEventVariant) {
  drm::input::KeyboardEvent ke;
  ke.time_ms = 100;
  ke.key = 42;
  ke.pressed = true;
  drm::input::InputEvent event{ke};

  EXPECT_TRUE(std::holds_alternative<drm::input::KeyboardEvent>(event));
  auto& got = std::get<drm::input::KeyboardEvent>(event);
  EXPECT_EQ(got.time_ms, 100U);
  EXPECT_EQ(got.key, 42U);
  EXPECT_TRUE(got.pressed);
}

TEST(InputEventTest, PointerMotionVariant) {
  drm::input::PointerMotionEvent me;
  me.time_ms = 50;
  me.dx = 1.5;
  me.dy = -2.0;
  drm::input::PointerEvent pe{me};
  drm::input::InputEvent const event{pe};

  EXPECT_TRUE(std::holds_alternative<drm::input::PointerEvent>(event));
}

TEST(InputEventTest, TouchEventVariant) {
  drm::input::TouchEvent te;
  te.time_ms = 200;
  te.slot = 0;
  te.x = 100.0;
  te.y = 200.0;
  te.type = drm::input::TouchEvent::Type::Down;
  drm::input::InputEvent const event{te};
  EXPECT_TRUE(std::holds_alternative<drm::input::TouchEvent>(event));
}

TEST(InputEventTest, SwitchEventVariant) {
  drm::input::SwitchEvent se;
  se.time_ms = 300;
  se.which = drm::input::SwitchEvent::Switch::Lid;
  se.active = true;
  drm::input::InputEvent const event{se};
  EXPECT_TRUE(std::holds_alternative<drm::input::SwitchEvent>(event));
}

// ── Pointer tests ─────────────────────────────────────────────

TEST(PointerTest, AccumulateMotion) {
  drm::input::Pointer ptr;
  EXPECT_DOUBLE_EQ(ptr.x(), 0.0);
  EXPECT_DOUBLE_EQ(ptr.y(), 0.0);

  ptr.accumulate_motion(10.0, -5.0);
  EXPECT_DOUBLE_EQ(ptr.x(), 10.0);
  EXPECT_DOUBLE_EQ(ptr.y(), -5.0);

  ptr.accumulate_motion(3.0, 2.0);
  EXPECT_DOUBLE_EQ(ptr.x(), 13.0);
  EXPECT_DOUBLE_EQ(ptr.y(), -3.0);
}

TEST(PointerTest, ButtonState) {
  drm::input::Pointer ptr;
  uint32_t const btn = 0x110;  // BTN_LEFT

  EXPECT_FALSE(ptr.button_pressed(btn));

  ptr.set_button(btn, true);
  EXPECT_TRUE(ptr.button_pressed(btn));

  ptr.set_button(btn, false);
  EXPECT_FALSE(ptr.button_pressed(btn));
}

TEST(PointerTest, ResetPosition) {
  drm::input::Pointer ptr;
  ptr.accumulate_motion(100.0, 200.0);
  ptr.reset_position(50.0, 50.0);

  EXPECT_DOUBLE_EQ(ptr.x(), 50.0);
  EXPECT_DOUBLE_EQ(ptr.y(), 50.0);
}

// ── EventDispatcher tests ─────────────────────────────────────

TEST(EventDispatcherTest, AddHandlerIncrementsCount) {
  drm::input::EventDispatcher dispatcher;
  EXPECT_EQ(dispatcher.handler_count(), 0U);

  dispatcher.add_handler([](const drm::input::InputEvent&) {});
  EXPECT_EQ(dispatcher.handler_count(), 1U);

  dispatcher.add_handler([](const drm::input::InputEvent&) {});
  EXPECT_EQ(dispatcher.handler_count(), 2U);
}

TEST(EventDispatcherTest, DispatchCallsAllHandlers) {
  drm::input::EventDispatcher dispatcher;
  int count1 = 0;
  int count2 = 0;

  dispatcher.add_handler([&](const drm::input::InputEvent&) { ++count1; });
  dispatcher.add_handler([&](const drm::input::InputEvent&) { ++count2; });

  drm::input::KeyboardEvent ke;
  ke.key = 1;
  ke.pressed = true;
  dispatcher.dispatch(drm::input::InputEvent{ke});

  EXPECT_EQ(count1, 1);
  EXPECT_EQ(count2, 1);
}

TEST(EventDispatcherTest, AsHandlerForwardsEvents) {
  drm::input::EventDispatcher dispatcher;
  int count = 0;

  dispatcher.add_handler([&](const drm::input::InputEvent&) { ++count; });

  auto handler = dispatcher.as_handler();
  drm::input::KeyboardEvent ke;
  ke.key = 1;
  ke.pressed = true;
  handler(drm::input::InputEvent{ke});

  EXPECT_EQ(count, 1);
}

// ── Keyboard tests ────────────────────────────────────────────

TEST(KeyboardTest, CreateWithDefaults) {
  auto result = drm::input::Keyboard::create();
  ASSERT_TRUE(result.has_value()) << "Failed to create keyboard with defaults";
}

TEST(KeyboardTest, CreateWithLayout) {
  auto result = drm::input::Keyboard::create({{}, {}, "us"});
  ASSERT_TRUE(result.has_value());
}

TEST(KeyboardTest, CreateFromInvalidFileFails) {
  auto result = drm::input::Keyboard::create_from_file("/nonexistent/keymap.xkb");
  EXPECT_FALSE(result.has_value());
}

TEST(KeyboardTest, ProcessKeyFillsSymAndUtf8) {
  auto kb_result = drm::input::Keyboard::create({{}, {}, "us"});
  ASSERT_TRUE(kb_result.has_value());
  auto& kb = *kb_result;

  // KEY_A = 30 in Linux input codes
  drm::input::KeyboardEvent ke;
  ke.key = 30;
  ke.pressed = true;
  kb.process_key(ke);

  // After processing, sym should be XKB_KEY_a (0x61) and utf8 should be "a"
  EXPECT_EQ(ke.sym, 0x61U);  // XKB_KEY_a
  EXPECT_STREQ(ke.utf8, "a");
}

TEST(KeyboardTest, ModifierStateAfterKeyRelease) {
  auto kb_result = drm::input::Keyboard::create({{}, {}, "us"});
  ASSERT_TRUE(kb_result.has_value());
  auto& kb = *kb_result;

  EXPECT_FALSE(kb.shift_active());
  EXPECT_FALSE(kb.ctrl_active());

  // Press shift (KEY_LEFTSHIFT = 42)
  drm::input::KeyboardEvent shift_down;
  shift_down.key = 42;
  shift_down.pressed = true;
  kb.process_key(shift_down);
  EXPECT_TRUE(kb.shift_active());

  // Release shift
  drm::input::KeyboardEvent shift_up;
  shift_up.key = 42;
  shift_up.pressed = false;
  kb.process_key(shift_up);
  EXPECT_FALSE(kb.shift_active());
}

// ── Seat tests ────────────────────────────────────────────────

TEST(SeatTest, OpenWithInvalidSeatFails) {
  // Opening a seat requires root privileges typically.
  // On CI without input devices, this should fail gracefully.
  auto result = drm::input::Seat::open({"nonexistent_seat_99"});
  // May succeed or fail depending on environment — just verify no crash.
  if (result.has_value()) {
    EXPECT_GE(result->fd(), 0);
  }
}
