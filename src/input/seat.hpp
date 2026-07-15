// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "keyboard.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cstdarg>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace drm::input {

// ── Event types ────────────────────────────────────────────────

struct KeyboardEvent {
  uint32_t time_ms{};
  uint32_t key{};  // Linux key code (KEY_*)
  bool pressed{};  // true = press, false = release
  bool repeat{};   // true if synthesized by KeyRepeater (sym/utf8 re-resolved)
  uint32_t sym{};  // XKB keysym (filled by Keyboard)
  char utf8[8]{};  // UTF-8 representation (filled by Keyboard)
};

struct PointerMotionEvent {
  uint32_t time_ms{};
  double dx{};
  double dy{};
  // libinput device name (libinput_device_get_name), or nullptr. Valid for the
  // duration of the synchronous EventHandler call only — the device owns the
  // string; copy it if retained. Lets a consumer apply per-device handling
  // (e.g. rotating a built-in trackpad's deltas to a rotated display while
  // leaving an external mouse alone).
  const char* device_name{nullptr};
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
  // libinput device name; see PointerMotionEvent::device_name for lifetime.
  const char* device_name{nullptr};
};

struct SwitchEvent {
  uint32_t time_ms{};
  enum class Switch : uint8_t { Lid, TabletMode } which{};
  bool active{};
};

using InputEvent = std::variant<KeyboardEvent, PointerEvent, TouchEvent, SwitchEvent>;
using EventHandler = std::function<void(const InputEvent&)>;

// ── Logging ────────────────────────────────────────────────────
//
// libinput emits its own diagnostics — device add/remove chatter,
// quirk-parse failures, `client bug: event processing lagging`. Left
// alone it sends them to its own stderr handler, escaping drm::log and
// any sink a consumer installed with drm::set_log_sink.
//
// Seat therefore always installs a libinput log handler. By default it
// forwards into drm::log with a `[libinput]` tag, so libinput's output
// follows set_log_sink like every other drm-cxx message. Supplying
// SeatOptions::log_handler overrides that and routes to the caller
// instead — for a consumer that wants libinput's stream separated from
// the library's own.

enum class LogPriority : uint8_t { Debug, Info, Error };

/// Sink for libinput's diagnostics. The message is already formatted and
/// stripped of libinput's trailing newline. The view is valid for the
/// duration of the synchronous call only — copy it if retained (same
/// contract as PointerMotionEvent::device_name).
using LogHandler = std::function<void(LogPriority, std::string_view)>;

// ── SeatOptions ────────────────────────────────────────────────

struct SeatOptions {
  std::string_view seat_name = "seat0";
  std::string_view keymap_path;  // Empty = use RMLVO defaults

  /// Sink for libinput's own diagnostics. Empty (the default) routes them
  /// into drm::log, tagged `[libinput]`, where drm::set_log_sink and
  /// drm::set_log_level apply. Set this only to divert libinput's stream
  /// somewhere other than the library's own.
  LogHandler log_handler;

  /// Threshold libinput itself applies before calling us at all. The
  /// default matches libinput's own, so the routing above costs nothing
  /// extra; raise it to Info/Debug to also collect device add/remove
  /// chatter and quirk diagnostics. Note drm::log's level gates on top of
  /// this when `log_handler` is empty — a message must pass both.
  LogPriority log_priority = LogPriority::Error;
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

// ── SeatContext ────────────────────────────────────────────────
//
// Everything libinput's C trampolines need to reach back into the
// owning Seat, bundled behind the single `user_data` slot libinput
// gives us: open/close_restricted receive it directly, and the log
// handler recovers it from its `struct libinput*` via
// libinput_get_user_data. Heap-allocated by `open` so its address
// survives Seat moves — libinput dereferences it on every device open.
// Opaque here; defined in seat.cpp.
struct SeatContext;

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

  // Push the given Caps/Num/Scroll Lock state to every keyboard-capable
  // libinput device on this seat. The xkb state has already been updated
  // by Keyboard::process_key — this writes the LED state back to the
  // kernel so the physical LEDs light up.
  void update_keyboard_leds(KeyboardLeds leds);

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
  // libinput's user_data. Heap-allocated so its address is stable
  // across Seat moves — see SeatContext.
  std::unique_ptr<SeatContext> ctx_;
  // Keyboard-capable libinput_device* (opaque). Tracked from
  // LIBINPUT_EVENT_DEVICE_ADDED/REMOVED so update_keyboard_leds can
  // iterate them. Each entry holds a libinput_device_ref; ~Seat unrefs.
  std::vector<void*> keyboard_devices_;
  // Last LED state pushed via update_keyboard_leds. Re-applied to any
  // newly-added keyboard device so VT-resume / hotplug doesn't leave
  // the physical LEDs lagging the xkb state. Empty until the first
  // update_keyboard_leds call.
  std::optional<KeyboardLeds> last_leds_;
  int fd_{-1};
};

// ── detail ─────────────────────────────────────────────────────
//
// Exposed for unit testing. Not part of the supported API.
namespace detail {

/// Maps a libinput priority (LIBINPUT_LOG_PRIORITY_*) onto LogPriority.
/// Takes an int rather than the enum so this header stays free of
/// <libinput.h> — session/seat.hpp includes us, so a leak here spreads.
/// Unknown values round toward Error: a diagnostic we can't classify is
/// worth surfacing, not dropping.
LogPriority map_log_priority(int libinput_priority) noexcept;

/// Formats `format`/`args`, strips libinput's trailing newline, and
/// forwards the result to `handler` at the mapped priority. No-op when
/// `handler` is empty or `format` is null. Does not consume `args` —
/// the caller retains ownership.
void dispatch_log(const LogHandler& handler, int libinput_priority, const char* format,
                  va_list args);

}  // namespace detail

}  // namespace drm::input
