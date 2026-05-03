// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// vt_switch.hpp — Ctrl+Alt+F<n> chord handling for examples.
//
// When seatd takes the TTY into KD_GRAPHICS, the kernel stops
// translating Ctrl+Alt+F<n> into a VT switch; the application has to
// drive it via libseat itself. Every example that owns both an
// `input::Seat` (libinput keyboard) and a `session::Seat` (libseat
// session) has the same boilerplate: track modifier state, map a
// function-key press to a VT number, call libseat_switch_session.
// This helper packages that.
//
// Usage:
//
//   drm::examples::VtChordTracker vt_chord;
//   input_seat.set_event_handler([&](const drm::input::InputEvent& ev) {
//     if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&ev)) {
//       if (vt_chord.observe(*ke, seat ? &*seat : nullptr)) {
//         return;  // chord or modifier — don't fall through
//       }
//       // ... example-specific handling (Esc/q, etc.)
//     }
//   });

#pragma once

#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/session/seat.hpp>

#include <linux/input-event-codes.h>

namespace drm::examples {

class VtChordTracker {
 public:
  /// Observe one keyboard event. Updates internal Ctrl/Alt modifier
  /// state and, on a Ctrl+Alt+F<n> press with a non-null `seat`,
  /// requests a VT switch. Returns true when the event is part of the
  /// chord machinery (modifier press/release or a fired chord) — the
  /// caller should early-return, so example-specific logic doesn't see
  /// it. Returns false for everything else.
  /// True when this press should terminate the example. Covers Esc, Q,
  /// and Ctrl+C — the last because libseat puts the seat into
  /// KD_GRAPHICS, where the kernel no longer translates ^C into SIGINT,
  /// so the libinput keyboard is the only path that still sees it.
  /// Modifier state comes from prior `observe` calls, so callers must
  /// invoke `observe` first and only consult this when `observe`
  /// returned false (i.e. the event isn't a chord component).
  [[nodiscard]] bool is_quit_key(const drm::input::KeyboardEvent& ke) const {
    if (!ke.pressed) {
      return false;
    }
    if (ke.key == KEY_ESC || ke.key == KEY_Q) {
      return true;
    }
    if (ke.key == KEY_C && ctrl_held()) {
      return true;
    }
    return false;
  }

  bool observe(const drm::input::KeyboardEvent& ke, drm::session::Seat* seat) {
    switch (ke.key) {
      case KEY_LEFTCTRL:
        ctrl_left_ = ke.pressed;
        return true;
      case KEY_RIGHTCTRL:
        ctrl_right_ = ke.pressed;
        return true;
      case KEY_LEFTALT:
        alt_left_ = ke.pressed;
        return true;
      case KEY_RIGHTALT:
        alt_right_ = ke.pressed;
        return true;
      default:
        break;
    }
    if (!ke.pressed || !ctrl_held() || !alt_held()) {
      return false;
    }
    int target_vt = 0;
    switch (ke.key) {
      case KEY_F1:
        target_vt = 1;
        break;
      case KEY_F2:
        target_vt = 2;
        break;
      case KEY_F3:
        target_vt = 3;
        break;
      case KEY_F4:
        target_vt = 4;
        break;
      case KEY_F5:
        target_vt = 5;
        break;
      case KEY_F6:
        target_vt = 6;
        break;
      case KEY_F7:
        target_vt = 7;
        break;
      case KEY_F8:
        target_vt = 8;
        break;
      case KEY_F9:
        target_vt = 9;
        break;
      case KEY_F10:
        target_vt = 10;
        break;
      case KEY_F11:
        target_vt = 11;
        break;
      case KEY_F12:
        target_vt = 12;
        break;
      default:
        return false;
    }
    if (seat != nullptr) {
      if (auto r = seat->switch_session(target_vt); !r) {
        drm::println(stderr, "switch_session(vt {}): {}", target_vt, r.error().message());
      }
    }
    return true;
  }

 private:
  // Track left/right modifiers separately. Pressing both then
  // releasing one must keep the modifier "held" while the other is
  // still down — a single bool flips false on the first release and
  // misses chords formed with the surviving key.
  [[nodiscard]] bool ctrl_held() const noexcept { return ctrl_left_ || ctrl_right_; }
  [[nodiscard]] bool alt_held() const noexcept { return alt_left_ || alt_right_; }

  bool ctrl_left_{false};
  bool ctrl_right_{false};
  bool alt_left_{false};
  bool alt_right_{false};
};

}  // namespace drm::examples
