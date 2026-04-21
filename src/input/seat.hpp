// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <functional>
#include <memory>
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
using EventHandler = std::function<void(const InputEvent&)>;

// ── SeatOptions ────────────────────────────────────────────────

struct SeatOptions {
  std::string_view seat_name = "seat0";
  std::string_view keymap_path;  // Empty = use RMLVO defaults
};

// ── InputDeviceOpener ──────────────────────────────────────────
//
// Hook for routing libinput's privileged device opens (and matching
// closes) through a seat/session manager instead of direct syscalls.
// The two callbacks correspond 1:1 to libinput's
// `open_restricted`/`close_restricted` contract. Empty callbacks mean
// "use ::open / ::close directly" — the default.
//
// Example: plugging a libseat-backed SeatSession in so every
// /dev/input/event* libinput touches is revoked on VT switch.
struct InputDeviceOpener {
  std::function<int(const char* path, int flags)> open;
  std::function<void(int fd)> close;

  [[nodiscard]] bool empty() const noexcept { return !open && !close; }
};

// ── Seat ───────────────────────────────────────────────────────

class Seat {
 public:
  /// Open the seat using the default opener (direct ::open/::close).
  static drm::expected<Seat, std::error_code> open(SeatOptions opts = {});

  /// Open the seat with a caller-provided opener. Use this to route
  /// libinput's privileged opens through libseat (or another session
  /// manager). Both callbacks must be set or both left empty — a
  /// mismatched pair rejects with std::errc::invalid_argument.
  static drm::expected<Seat, std::error_code> open(SeatOptions opts, InputDeviceOpener opener);

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
  // Heap-allocated so its address is stable across Seat moves;
  // libinput's open/close_restricted callbacks get this pointer via
  // user_data and dereference it on every device open.
  std::unique_ptr<InputDeviceOpener> opener_;
  int fd_{-1};
};

}  // namespace drm::input
