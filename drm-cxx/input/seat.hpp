// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace drm::input {

// ── Event types ────────────────────────────────────────────────

struct KeyboardEvent {
  uint32_t time_ms{};
  uint32_t key{};  // Linux key code (KEY_*)
  bool pressed{};  // true = press, false = release
  uint32_t sym{};  // XKB keysym (filled by Keyboard)
  char utf8[8]{};  // UTF-8 representation (filled by Keyboard)
};

struct PointerMotionEvent {
  uint32_t time_ms{};
  double dx{};
  double dy{};
};

struct PointerButtonEvent {
  uint32_t time_ms{};
  uint32_t button{};  // BTN_LEFT, BTN_RIGHT, etc.
  bool pressed{};
};

struct PointerAxisEvent {
  uint32_t time_ms{};
  double horizontal{};
  double vertical{};
};

using PointerEvent = std::variant<PointerMotionEvent, PointerButtonEvent, PointerAxisEvent>;

struct TouchEvent {
  uint32_t time_ms{};
  int32_t slot{};  // Multi-touch slot
  double x{};
  double y{};
  enum class Type : uint8_t { Down, Up, Motion, Frame, Cancel } type{};
};

struct SwitchEvent {
  uint32_t time_ms{};
  enum class Switch : uint8_t { Lid, TabletMode } which{};
  bool active{};
};

using InputEvent = std::variant<KeyboardEvent, PointerEvent, TouchEvent, SwitchEvent>;
using EventHandler = std::move_only_function<void(const InputEvent&)>;

// ── SeatOptions ────────────────────────────────────────────────

struct SeatOptions {
  std::string_view seat_name = "seat0";
  std::string_view keymap_path;  // Empty = use RMLVO defaults
};

// ── Seat ───────────────────────────────────────────────────────

class Seat {
 public:
  static drm::expected<Seat, std::error_code> open(SeatOptions opts = {});

  void set_event_handler(EventHandler handler);

  // Dispatch pending libinput events. Call after poll/epoll on fd().
  drm::expected<void, std::error_code> dispatch();

  // File descriptor for poll/epoll integration.
  [[nodiscard]] int fd() const noexcept;

  // Suspend/resume for VT switching.
  drm::expected<void, std::error_code> suspend();
  drm::expected<void, std::error_code> resume();

  ~Seat();
  Seat(Seat&& /*other*/) noexcept;
  Seat& operator=(Seat&& /*other*/) noexcept;
  Seat(const Seat&) = delete;
  Seat& operator=(const Seat&) = delete;

 private:
  Seat() = default;
  void process_events();

  // Opaque pointers — actual types from libinput.h / libudev.h
  void* li_{};
  void* udev_{};
  EventHandler handler_;
  int fd_{-1};
};

}  // namespace drm::input
