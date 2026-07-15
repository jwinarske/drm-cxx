// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "input/event_dispatcher.hpp"
#include "input/keyboard.hpp"
#include "input/pointer.hpp"
#include "input/seat.hpp"
#include "log.hpp"

#include <xkbcommon/xkbcommon.h>

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

TEST(KeyboardTest, CapsLockLatchesAndExposesLedState) {
  auto kb_result = drm::input::Keyboard::create({{}, {}, "us"});
  ASSERT_TRUE(kb_result.has_value());
  auto& kb = *kb_result;

  EXPECT_FALSE(kb.caps_lock_active());
  EXPECT_FALSE(kb.leds_state().caps_lock);

  // Press + release Caps Lock (KEY_CAPSLOCK = 58). xkb toggles the
  // Lock modifier on press; release is a no-op for the latch.
  drm::input::KeyboardEvent caps_down;
  caps_down.key = 58;
  caps_down.pressed = true;
  kb.process_key(caps_down);
  drm::input::KeyboardEvent caps_up;
  caps_up.key = 58;
  caps_up.pressed = false;
  kb.process_key(caps_up);

  EXPECT_TRUE(kb.caps_lock_active());
  EXPECT_TRUE(kb.leds_state().caps_lock);
  EXPECT_FALSE(kb.leds_state().num_lock);
  EXPECT_FALSE(kb.leds_state().scroll_lock);

  // 'a' now resolves to 'A' because the Lock modifier is effective.
  drm::input::KeyboardEvent a_down;
  a_down.key = 30;  // KEY_A
  a_down.pressed = true;
  kb.process_key(a_down);
  EXPECT_STREQ(a_down.utf8, "A");

  // A second toggle clears the latch.
  drm::input::KeyboardEvent caps_down2 = caps_down;
  drm::input::KeyboardEvent caps_up2 = caps_up;
  kb.process_key(caps_down2);
  kb.process_key(caps_up2);
  EXPECT_FALSE(kb.caps_lock_active());
  EXPECT_FALSE(kb.leds_state().caps_lock);
}

TEST(KeyboardTest, LedsStateEqualityDetectsTransitions) {
  drm::input::KeyboardLeds const a;
  drm::input::KeyboardLeds b;
  EXPECT_EQ(a, b);
  b.caps_lock = true;
  EXPECT_NE(a, b);
}

namespace {

// Round-trip helper: serialize a "us" RMLVO keymap to its XKB v1 text
// form so we can feed it to Keyboard::create_from_string. Uses xkb
// directly because Keyboard intentionally doesn't expose a serializer.
std::string serialize_us_keymap() {
  auto* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  EXPECT_NE(ctx, nullptr);
  xkb_rule_names const names{nullptr, nullptr, "us", nullptr, nullptr};
  auto* keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  EXPECT_NE(keymap, nullptr);
  char* raw = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  std::string out(raw);
  std::free(raw);
  xkb_keymap_unref(keymap);
  xkb_context_unref(ctx);
  return out;
}

}  // namespace

TEST(KeyboardTest, CreateFromStringRoundTripsUs) {
  std::string const buffer = serialize_us_keymap();
  ASSERT_FALSE(buffer.empty());

  auto kb_result = drm::input::Keyboard::create_from_string(buffer);
  ASSERT_TRUE(kb_result.has_value());
  auto& kb = *kb_result;

  drm::input::KeyboardEvent ke;
  ke.key = 30;  // KEY_A
  ke.pressed = true;
  kb.process_key(ke);
  EXPECT_EQ(ke.sym, 0x61U);
  EXPECT_STREQ(ke.utf8, "a");
}

TEST(KeyboardTest, CreateFromStringRejectsGarbage) {
  auto kb_result = drm::input::Keyboard::create_from_string("not a keymap");
  EXPECT_FALSE(kb_result.has_value());
}

TEST(KeyboardTest, SetLedsDrivesCapsAndNumLatches) {
  auto kb_result = drm::input::Keyboard::create({{}, {}, "us"});
  ASSERT_TRUE(kb_result.has_value());
  auto& kb = *kb_result;

  EXPECT_FALSE(kb.caps_lock_active());
  EXPECT_FALSE(kb.num_lock_active());

  kb.set_leds({true, true, false});
  EXPECT_TRUE(kb.caps_lock_active());
  EXPECT_TRUE(kb.num_lock_active());

  // Idempotent — setting the same target leaves the latches untouched.
  kb.set_leds({true, true, false});
  EXPECT_TRUE(kb.caps_lock_active());
  EXPECT_TRUE(kb.num_lock_active());

  kb.set_leds({false, false, false});
  EXPECT_FALSE(kb.caps_lock_active());
  EXPECT_FALSE(kb.num_lock_active());
}

TEST(KeyboardTest, ReloadPreservesHeldShiftLevel) {
  auto kb_result = drm::input::Keyboard::create({{}, {}, "us"});
  ASSERT_TRUE(kb_result.has_value());
  auto& kb = *kb_result;

  // Press Shift, leave it held across the reload.
  drm::input::KeyboardEvent shift_down;
  shift_down.key = 42;  // KEY_LEFTSHIFT
  shift_down.pressed = true;
  kb.process_key(shift_down);
  ASSERT_TRUE(kb.shift_active());

  ASSERT_TRUE(kb.reload({{}, {}, "us"}).has_value());

  // After reload, Shift should still be effective — typing 'a' yields 'A'.
  EXPECT_TRUE(kb.shift_active());
  drm::input::KeyboardEvent a_down;
  a_down.key = 30;  // KEY_A
  a_down.pressed = true;
  kb.process_key(a_down);
  EXPECT_STREQ(a_down.utf8, "A");

  // Releasing Shift after the reload should drop back to lowercase.
  drm::input::KeyboardEvent shift_up;
  shift_up.key = 42;
  shift_up.pressed = false;
  kb.process_key(shift_up);
  EXPECT_FALSE(kb.shift_active());
}

TEST(KeyboardTest, ReloadPreservesCapsLockLatch) {
  auto kb_result = drm::input::Keyboard::create({{}, {}, "us"});
  ASSERT_TRUE(kb_result.has_value());
  auto& kb = *kb_result;

  drm::input::KeyboardEvent caps_down;
  caps_down.key = 58;  // KEY_CAPSLOCK
  caps_down.pressed = true;
  drm::input::KeyboardEvent caps_up;
  caps_up.key = 58;
  caps_up.pressed = false;
  kb.process_key(caps_down);
  kb.process_key(caps_up);
  ASSERT_TRUE(kb.caps_lock_active());

  ASSERT_TRUE(kb.reload({{}, {}, "us"}).has_value());

  EXPECT_TRUE(kb.caps_lock_active());
  drm::input::KeyboardEvent a_down;
  a_down.key = 30;
  a_down.pressed = true;
  kb.process_key(a_down);
  EXPECT_STREQ(a_down.utf8, "A");
}

TEST(KeyboardTest, ReloadWithBogusOptsLeavesKeymapIntact) {
  auto kb_result = drm::input::Keyboard::create({{}, {}, "us"});
  ASSERT_TRUE(kb_result.has_value());
  auto& kb = *kb_result;

  auto bad = kb.reload({{}, {}, "totally-not-a-real-layout-zzzz"});
  EXPECT_FALSE(bad.has_value());

  // Original "us" layout still works.
  drm::input::KeyboardEvent a_down;
  a_down.key = 30;
  a_down.pressed = true;
  kb.process_key(a_down);
  EXPECT_STREQ(a_down.utf8, "a");
}

// ── Seat log routing ──────────────────────────────────────────
//
// The libinput priority values below are spelled as the literals from
// libinput.h (DEBUG=10, INFO=20, ERROR=30) rather than the enum, so the
// test keeps <libinput.h> out and would notice an ABI renumber.

namespace {

constexpr int li_debug = 10;
constexpr int li_info = 20;
constexpr int li_error = 30;

// dispatch_log takes a va_list, and only a C-style variadic function can
// produce one — which is the whole point: this stands in for libinput's
// own printf-style call site.
// NOLINTNEXTLINE(modernize-avoid-variadic-functions) — mirrors libinput's ABI
void call_dispatch(const drm::input::LogHandler& handler, int priority, const char* format, ...) {
  va_list args;
  va_start(args, format);
  drm::input::detail::dispatch_log(handler, priority, format, args);
  va_end(args);
}

}  // namespace

TEST(SeatLogTest, MapsEachLibinputPriority) {
  using drm::input::LogPriority;
  EXPECT_EQ(drm::input::detail::map_log_priority(li_debug), LogPriority::Debug);
  EXPECT_EQ(drm::input::detail::map_log_priority(li_info), LogPriority::Info);
  EXPECT_EQ(drm::input::detail::map_log_priority(li_error), LogPriority::Error);
}

TEST(SeatLogTest, MapsUnknownPrioritiesToNearestBand) {
  using drm::input::LogPriority;
  // Between the named values — round up to the next named band, so an
  // unclassifiable message errs toward being seen rather than dropped.
  EXPECT_EQ(drm::input::detail::map_log_priority(15), LogPriority::Info);
  EXPECT_EQ(drm::input::detail::map_log_priority(25), LogPriority::Error);
  // Below DEBUG and above ERROR: clamp rather than drop.
  EXPECT_EQ(drm::input::detail::map_log_priority(0), LogPriority::Debug);
  EXPECT_EQ(drm::input::detail::map_log_priority(999), LogPriority::Error);
}

TEST(SeatLogTest, ExpandsVarargsAndForwardsPriority) {
  drm::input::LogPriority got_priority{};
  std::string got_msg;
  drm::input::LogHandler const handler = [&](drm::input::LogPriority p, std::string_view msg) {
    got_priority = p;
    got_msg = msg;
  };

  call_dispatch(handler, li_error, "client bug: event processing lagging by %dms (%s)\n", 42,
                "pointer");

  EXPECT_EQ(got_priority, drm::input::LogPriority::Error);
  EXPECT_EQ(got_msg, "client bug: event processing lagging by 42ms (pointer)");
}

TEST(SeatLogTest, StripsLibinputTrailingNewline) {
  std::string got_msg;
  drm::input::LogHandler const handler = [&](drm::input::LogPriority, std::string_view msg) {
    got_msg = msg;
  };

  call_dispatch(handler, li_info, "added device\n");
  EXPECT_EQ(got_msg, "added device");

  // Interior newlines are the message's own business — only strip the tail.
  call_dispatch(handler, li_info, "line one\nline two\n");
  EXPECT_EQ(got_msg, "line one\nline two");
}

// RAII sink capture: drm::set_log_sink is process-global, so restore it (and
// the level) however the test exits, or a failure here silently reroutes every
// later test's logging.
namespace {
class ScopedLogCapture {
 public:
  explicit ScopedLogCapture(drm::LogLevel level = drm::LogLevel::Debug)
      : prev_level_(drm::get_log_level()) {
    drm::set_log_level(level);
    drm::set_log_sink([this](drm::LogLevel lvl, std::string_view msg) {
      lines_.emplace_back(lvl, std::string(msg));
    });
  }
  ~ScopedLogCapture() {
    drm::set_log_sink(nullptr);
    drm::set_log_level(prev_level_);
  }
  ScopedLogCapture(const ScopedLogCapture&) = delete;
  ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;
  ScopedLogCapture(ScopedLogCapture&&) = delete;
  ScopedLogCapture& operator=(ScopedLogCapture&&) = delete;

  [[nodiscard]] const std::vector<std::pair<drm::LogLevel, std::string>>& lines() const {
    return lines_;
  }

 private:
  drm::LogLevel prev_level_;
  std::vector<std::pair<drm::LogLevel, std::string>> lines_;
};
}  // namespace

TEST(SeatLogTest, EmptyHandlerRoutesIntoDrmLog) {
  ScopedLogCapture cap;
  drm::input::LogHandler const empty;

  call_dispatch(empty, li_error, "client bug: event processing lagging by %dms", 42);

  ASSERT_EQ(cap.lines().size(), 1U);
  EXPECT_EQ(cap.lines()[0].first, drm::LogLevel::Error);
  // Tagged so a consumer can tell libinput's stream from drm-cxx's own.
  EXPECT_EQ(cap.lines()[0].second, "[libinput] client bug: event processing lagging by 42ms");
}

TEST(SeatLogTest, EmptyHandlerMapsPriorityOntoDrmLogLevel) {
  ScopedLogCapture cap;
  drm::input::LogHandler const empty;

  call_dispatch(empty, li_debug, "added device");
  call_dispatch(empty, li_info, "quirks loaded");
  call_dispatch(empty, li_error, "boom");

  ASSERT_EQ(cap.lines().size(), 3U);
  EXPECT_EQ(cap.lines()[0].first, drm::LogLevel::Debug);
  EXPECT_EQ(cap.lines()[1].first, drm::LogLevel::Info);
  EXPECT_EQ(cap.lines()[2].first, drm::LogLevel::Error);
}

TEST(SeatLogTest, SuppliedHandlerOverridesDrmLog) {
  ScopedLogCapture cap;
  std::string got;
  drm::input::LogHandler const handler = [&](drm::input::LogPriority, std::string_view msg) {
    got = msg;
  };

  call_dispatch(handler, li_error, "to the caller");

  EXPECT_EQ(got, "to the caller");
  // The caller asked for libinput's stream separately; it must not ALSO be
  // duplicated into drm::log.
  EXPECT_TRUE(cap.lines().empty());
}

TEST(SeatLogTest, DrmLogLevelGatesTheDefaultRoute) {
  ScopedLogCapture cap(drm::LogLevel::Error);
  drm::input::LogHandler const empty;

  call_dispatch(empty, li_debug, "chatty");
  call_dispatch(empty, li_error, "kept");

  ASSERT_EQ(cap.lines().size(), 1U);
  EXPECT_EQ(cap.lines()[0].second, "[libinput] kept");
}

TEST(SeatLogTest, OverlongMessageIsTruncatedNotOverrun) {
  std::string got_msg;
  drm::input::LogHandler const handler = [&](drm::input::LogPriority, std::string_view msg) {
    got_msg = msg;
  };

  std::string const huge(4096, 'x');
  call_dispatch(handler, li_error, "%s", huge.c_str());

  // Clamped to the internal buffer rather than reporting vsnprintf's
  // would-have-written length, which would over-read.
  EXPECT_LT(got_msg.size(), huge.size());
  EXPECT_EQ(got_msg, std::string(got_msg.size(), 'x'));
}

TEST(SeatLogTest, DefaultOptionsRouteIntoDrmLogAtErrorThreshold) {
  drm::input::SeatOptions const opts;
  // No caller sink => the trampoline forwards into drm::log.
  EXPECT_FALSE(opts.log_handler);
  // Matches libinput's own default threshold, so the routing adds no chatter.
  EXPECT_EQ(opts.log_priority, drm::input::LogPriority::Error);
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
