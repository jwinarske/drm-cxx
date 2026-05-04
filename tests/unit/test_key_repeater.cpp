// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "input/key_repeater.hpp"
#include "input/keyboard.hpp"
#include "input/seat.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <linux/input-event-codes.h>
#include <sys/poll.h>
#include <utility>
#include <vector>

namespace {

drm::input::Keyboard make_us_keyboard() {
  auto kb = drm::input::Keyboard::create({{}, {}, "us"});
  EXPECT_TRUE(kb.has_value());
  return std::move(*kb);
}

bool wait_readable(int fd, int timeout_ms) {
  pollfd pfd{fd, POLLIN, 0};
  int const rc = ::poll(&pfd, 1, timeout_ms);
  return rc > 0 && (pfd.revents & POLLIN) != 0;
}

drm::input::KeyboardEvent make_press(uint32_t key) {
  drm::input::KeyboardEvent ke{};
  ke.key = key;
  ke.pressed = true;
  return ke;
}

drm::input::KeyboardEvent make_release(uint32_t key) {
  drm::input::KeyboardEvent ke{};
  ke.key = key;
  ke.pressed = false;
  return ke;
}

}  // namespace

TEST(KeyboardRepeatPredicate, LettersRepeatModifiersDoNot) {
  auto const kb = make_us_keyboard();
  EXPECT_TRUE(kb.should_repeat(KEY_A));
  EXPECT_TRUE(kb.should_repeat(KEY_LEFT));
  EXPECT_TRUE(kb.should_repeat(KEY_F1));
  EXPECT_FALSE(kb.should_repeat(KEY_LEFTSHIFT));
  EXPECT_FALSE(kb.should_repeat(KEY_LEFTCTRL));
  EXPECT_FALSE(kb.should_repeat(KEY_LEFTALT));
  EXPECT_FALSE(kb.should_repeat(KEY_CAPSLOCK));
}

TEST(KeyRepeaterCreate, RejectsNullKeyboard) {
  auto r = drm::input::KeyRepeater::create(nullptr);
  EXPECT_FALSE(r.has_value());
}

TEST(KeyRepeaterCreate, RejectsZeroInterval) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb, {600, 0});
  EXPECT_FALSE(r.has_value());
}

TEST(KeyRepeaterCreate, ProvidesValidFd) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb);
  ASSERT_TRUE(r.has_value());
  EXPECT_GE(r->fd(), 0);
}

TEST(KeyRepeaterState, PressTracksHeldKey) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb).value();

  EXPECT_EQ(r.held_key(), 0U);
  r.on_key(make_press(KEY_A));
  EXPECT_EQ(r.held_key(), KEY_A);
}

TEST(KeyRepeaterState, ReleaseClearsHeldKey) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb).value();

  r.on_key(make_press(KEY_A));
  ASSERT_EQ(r.held_key(), KEY_A);
  r.on_key(make_release(KEY_A));
  EXPECT_EQ(r.held_key(), 0U);
}

TEST(KeyRepeaterState, NonRepeatingKeyDoesNotDisturbHold) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb).value();

  r.on_key(make_press(KEY_A));
  ASSERT_EQ(r.held_key(), KEY_A);
  r.on_key(make_press(KEY_LEFTSHIFT));
  EXPECT_EQ(r.held_key(), KEY_A);
  r.on_key(make_release(KEY_LEFTSHIFT));
  EXPECT_EQ(r.held_key(), KEY_A);
}

TEST(KeyRepeaterState, NewRepeatingKeyReplacesPriorHold) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb).value();

  r.on_key(make_press(KEY_A));
  ASSERT_EQ(r.held_key(), KEY_A);
  r.on_key(make_press(KEY_B));
  EXPECT_EQ(r.held_key(), KEY_B);
}

TEST(KeyRepeaterState, ReleaseOfNonHeldKeyIsNoop) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb).value();

  r.on_key(make_press(KEY_A));
  r.on_key(make_release(KEY_B));
  EXPECT_EQ(r.held_key(), KEY_A);
}

TEST(KeyRepeaterState, CancelClearsHeldKey) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb).value();

  r.on_key(make_press(KEY_A));
  ASSERT_EQ(r.held_key(), KEY_A);
  r.cancel();
  EXPECT_EQ(r.held_key(), 0U);
}

TEST(KeyRepeaterState, SynthesizedRepeatEventIsIgnored) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb).value();

  drm::input::KeyboardEvent fake_repeat{};
  fake_repeat.key = KEY_A;
  fake_repeat.pressed = true;
  fake_repeat.repeat = true;
  r.on_key(fake_repeat);
  EXPECT_EQ(r.held_key(), 0U);
}

TEST(KeyRepeaterEmit, FiresAfterDelayWithRepeatFlag) {
  auto const kb = make_us_keyboard();
  // Short delay/interval for a fast deterministic test.
  auto r = drm::input::KeyRepeater::create(&kb, {30, 30}).value();

  std::vector<drm::input::KeyboardEvent> received;
  r.set_handler([&](const drm::input::KeyboardEvent& ke) { received.push_back(ke); });

  r.on_key(make_press(KEY_A));

  ASSERT_TRUE(wait_readable(r.fd(), /*timeout_ms=*/200));
  r.dispatch();

  ASSERT_FALSE(received.empty());
  auto const& ke = received.front();
  EXPECT_EQ(ke.key, KEY_A);
  EXPECT_TRUE(ke.pressed);
  EXPECT_TRUE(ke.repeat);
  EXPECT_EQ(ke.sym, 0x61U);  // XKB_KEY_a — process_key ran during dispatch
  EXPECT_STREQ(ke.utf8, "a");
}

TEST(KeyRepeaterEmit, NoFireAfterRelease) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb, {30, 30}).value();

  int calls = 0;
  r.set_handler([&](const drm::input::KeyboardEvent&) { ++calls; });

  r.on_key(make_press(KEY_A));
  r.on_key(make_release(KEY_A));

  EXPECT_FALSE(wait_readable(r.fd(), /*timeout_ms=*/100));
  r.dispatch();
  EXPECT_EQ(calls, 0);
}

TEST(KeyRepeaterEmit, NoFireAfterCancel) {
  auto const kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb, {30, 30}).value();

  int calls = 0;
  r.set_handler([&](const drm::input::KeyboardEvent&) { ++calls; });

  r.on_key(make_press(KEY_A));
  r.cancel();

  EXPECT_FALSE(wait_readable(r.fd(), /*timeout_ms=*/100));
  r.dispatch();
  EXPECT_EQ(calls, 0);
}

TEST(KeyRepeaterEmit, ShiftHeldDuringRepeatProducesUppercase) {
  // Mutable keyboard so process_key can update modifier state.
  auto kb = make_us_keyboard();
  auto r = drm::input::KeyRepeater::create(&kb, {30, 30}).value();

  std::vector<drm::input::KeyboardEvent> received;
  r.set_handler([&](const drm::input::KeyboardEvent& ke) { received.push_back(ke); });

  // Press shift first to set modifier state, then press 'a' to start repeat.
  drm::input::KeyboardEvent shift = make_press(KEY_LEFTSHIFT);
  kb.process_key(shift);
  ASSERT_TRUE(kb.shift_active());

  r.on_key(make_press(KEY_A));

  ASSERT_TRUE(wait_readable(r.fd(), /*timeout_ms=*/200));
  r.dispatch();

  ASSERT_FALSE(received.empty());
  // sym + utf8 re-resolved against current xkb state (shift held) → 'A'.
  EXPECT_EQ(received.front().sym, 0x41U);  // XKB_KEY_A
  EXPECT_STREQ(received.front().utf8, "A");
}