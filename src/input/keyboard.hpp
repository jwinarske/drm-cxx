// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <string_view>
#include <system_error>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace drm::input {

struct KeyboardEvent;

struct KeymapOptions {
  std::string_view rules;
  std::string_view model;
  std::string_view layout;
  std::string_view variant;
  std::string_view options;
};

// Snapshot of the xkb-tracked LED state (Caps / Num / Scroll Lock).
// Returned by `Keyboard::leds_state()` and consumed by
// `Seat::update_keyboard_leds()` to drive the physical keyboard LEDs.
struct KeyboardLeds {
  bool caps_lock{};
  bool num_lock{};
  bool scroll_lock{};

  friend bool operator==(KeyboardLeds a, KeyboardLeds b) noexcept {
    return a.caps_lock == b.caps_lock && a.num_lock == b.num_lock && a.scroll_lock == b.scroll_lock;
  }
  friend bool operator!=(KeyboardLeds a, KeyboardLeds b) noexcept { return !(a == b); }
};

class Keyboard {
 public:
  // Create from RMLVO names (empty = system defaults).
  static drm::expected<Keyboard, std::error_code> create(KeymapOptions opts = {});

  // Create from a keymap file path (e.g. $HOME/.xkb/keymap.xkb).
  static drm::expected<Keyboard, std::error_code> create_from_file(std::string_view keymap_path);

  // Process a key event: fills in sym and utf8 fields.
  void process_key(KeyboardEvent& event) const;

  // True if the keymap marks this Linux keycode (KEY_*) as auto-repeating.
  // Modifiers and lock keys naturally return false; letters / digits /
  // arrow keys / function keys naturally return true.
  [[nodiscard]] bool should_repeat(uint32_t key) const noexcept;

  // Query modifier state.
  [[nodiscard]] bool shift_active() const noexcept;
  [[nodiscard]] bool ctrl_active() const noexcept;
  [[nodiscard]] bool alt_active() const noexcept;
  [[nodiscard]] bool super_active() const noexcept;

  // Query lock state. xkb tracks these in the Lock modifier; the
  // accessors here read the named LED state which mirrors the lock-mod
  // state for caps/num/scroll on standard layouts.
  [[nodiscard]] bool caps_lock_active() const noexcept;
  [[nodiscard]] bool num_lock_active() const noexcept;
  [[nodiscard]] bool scroll_lock_active() const noexcept;

  // Snapshot of the LED state. Compare two snapshots with operator!= to
  // detect a transition, then push the new state to the seat with
  // Seat::update_keyboard_leds().
  [[nodiscard]] KeyboardLeds leds_state() const noexcept;

  ~Keyboard();
  Keyboard(Keyboard&& /*other*/) noexcept;
  Keyboard& operator=(Keyboard&& /*other*/) noexcept;
  Keyboard(const Keyboard&) = delete;
  Keyboard& operator=(const Keyboard&) = delete;

 private:
  Keyboard() = default;

  struct xkb_context* ctx_{};
  struct xkb_keymap* keymap_{};
  struct xkb_state* state_{};
};

}  // namespace drm::input
