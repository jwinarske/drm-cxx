// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <functional>
#include <system_error>

namespace drm::input {

class Keyboard;
struct KeyboardEvent;

struct RepeatConfig {
  uint32_t delay_ms{600};    // X11/Wayland-conventional initial delay.
  uint32_t interval_ms{40};  // 25 Hz default (1000 / 40).
};

/// Synthesizes auto-repeat KeyboardEvents for held keys.
///
/// libinput does not auto-repeat by design — Wayland compositors do it
/// themselves. This class is the equivalent for drm-cxx: feed every real
/// KeyboardEvent into `on_key`, register `fd()` on the same epoll the
/// rest of the input loop uses, and call `dispatch()` when the fd is
/// readable. Synthesized events arrive via the registered handler with
/// `KeyboardEvent::repeat == true`; `sym` and `utf8` are re-resolved
/// against the current xkb state on every tick, so modifier changes
/// during the hold (Shift, AltGr, level switches) take effect on the
/// next repeat without a restart.
///
/// Per-key repeat eligibility is decided by xkb (`should_repeat`):
/// modifiers and lock keys do not repeat; letters / digits / arrow keys
/// / function keys do.
class KeyRepeater {
 public:
  using Handler = std::function<void(const KeyboardEvent&)>;

  /// `keyboard` must outlive this repeater (non-owning). Used for
  /// `should_repeat` lookups and `process_key` re-resolution.
  static drm::expected<KeyRepeater, std::error_code> create(const Keyboard* keyboard,
                                                            RepeatConfig cfg = {});

  void set_handler(Handler handler);

  /// Feed a real KeyboardEvent. Synthesized events (event.repeat == true)
  /// are ignored — do not loop the handler's output back in here.
  void on_key(const KeyboardEvent& event);

  /// Drain timerfd expirations and emit one synthesized event per tick
  /// through the handler. No-op if the handler is unset or no key is held.
  void dispatch() const;

  /// Cancel any in-flight repeat. Call on session pause or explicit reset.
  void cancel();

  /// timerfd for poll/epoll integration. -1 if unavailable.
  [[nodiscard]] int fd() const noexcept;

  /// Currently tracked held key (0 if none). Mostly useful for tests.
  [[nodiscard]] uint32_t held_key() const noexcept;

  ~KeyRepeater();
  KeyRepeater(KeyRepeater&& other) noexcept;
  KeyRepeater& operator=(KeyRepeater&& other) noexcept;
  KeyRepeater(const KeyRepeater&) = delete;
  KeyRepeater& operator=(const KeyRepeater&) = delete;

 private:
  KeyRepeater() = default;
  void arm() const;
  void disarm() const;
  void close_timer_fd();

  const Keyboard* keyboard_{nullptr};
  RepeatConfig cfg_{};
  Handler handler_;
  int timer_fd_{-1};
  uint32_t held_key_{0};
  bool is_held_{false};
};

}  // namespace drm::input