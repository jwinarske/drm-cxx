// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "key_repeater.hpp"

#include "keyboard.hpp"
#include "seat.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <system_error>
#include <time.h>  // NOLINT(modernize-deprecated-headers) — POSIX itimerspec/CLOCK_MONOTONIC live here, not in <ctime>
#include <unistd.h>
#include <utility>

namespace drm::input {

namespace {

constexpr uint64_t k_ns_per_ms = 1'000'000ULL;
constexpr uint64_t k_ns_per_sec = 1'000'000'000ULL;

// itimerspec is declared via <time.h> (POSIX); libc spreads its definition
// across <bits/types/struct_itimerspec.h>, which include-cleaner can't trace
// back to the canonical header.
// NOLINTNEXTLINE(misc-include-cleaner)
itimerspec make_spec(uint32_t delay_ms, uint32_t interval_ms) {
  itimerspec spec{};
  uint64_t delay_ns = static_cast<uint64_t>(delay_ms) * k_ns_per_ms;
  uint64_t const interval_ns = static_cast<uint64_t>(interval_ms) * k_ns_per_ms;
  // timerfd_settime treats it_value == {0, 0} as "disarm", which would
  // silently disable repeats when the caller asked for delay_ms == 0
  // ("fire immediately"). Clamp to 1 ns so the timer arms and the first
  // expiration lands on the next dispatch tick.
  if (delay_ns == 0) {
    delay_ns = 1;
  }
  spec.it_value.tv_sec = static_cast<time_t>(delay_ns / k_ns_per_sec);
  spec.it_value.tv_nsec = static_cast<long>(delay_ns % k_ns_per_sec);
  spec.it_interval.tv_sec = static_cast<time_t>(interval_ns / k_ns_per_sec);
  spec.it_interval.tv_nsec = static_cast<long>(interval_ns % k_ns_per_sec);
  return spec;
}

uint32_t monotonic_ms() {
  using namespace std::chrono;
  auto const now = steady_clock::now().time_since_epoch();
  return static_cast<uint32_t>(duration_cast<milliseconds>(now).count());
}

}  // namespace

drm::expected<KeyRepeater, std::error_code> KeyRepeater::create(const Keyboard* keyboard,
                                                                RepeatConfig cfg) {
  if (keyboard == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  // delay_ms == 0 disables the initial wait — pathological but legal;
  // interval_ms == 0 would tight-loop the timer, so reject it.
  if (cfg.interval_ms == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  // NOLINTNEXTLINE(misc-include-cleaner) — CLOCK_MONOTONIC declared via <time.h> transitively
  int const tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tfd < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  KeyRepeater r;
  r.keyboard_ = keyboard;
  r.cfg_ = cfg;
  r.timer_fd_ = tfd;
  return r;
}

KeyRepeater::~KeyRepeater() {
  close_timer_fd();
}

KeyRepeater::KeyRepeater(KeyRepeater&& other) noexcept
    : keyboard_(other.keyboard_),
      cfg_(other.cfg_),
      handler_(std::move(other.handler_)),
      timer_fd_(other.timer_fd_),
      held_key_(other.held_key_),
      is_held_(other.is_held_) {
  other.keyboard_ = nullptr;
  other.timer_fd_ = -1;
  other.held_key_ = 0;
  other.is_held_ = false;
}

KeyRepeater& KeyRepeater::operator=(KeyRepeater&& other) noexcept {
  if (this != &other) {
    close_timer_fd();
    keyboard_ = other.keyboard_;
    cfg_ = other.cfg_;
    handler_ = std::move(other.handler_);
    timer_fd_ = other.timer_fd_;
    held_key_ = other.held_key_;
    is_held_ = other.is_held_;

    other.keyboard_ = nullptr;
    other.timer_fd_ = -1;
    other.held_key_ = 0;
    other.is_held_ = false;
  }
  return *this;
}

void KeyRepeater::set_handler(Handler handler) {
  handler_ = std::move(handler);
}

void KeyRepeater::on_key(const KeyboardEvent& event) {
  // Don't recurse on our own synthesized events.
  if (event.repeat) {
    return;
  }

  if (event.pressed) {
    if (keyboard_ == nullptr || !keyboard_->should_repeat(event.key)) {
      // Modifier or another non-repeating key: doesn't disturb an in-flight
      // repeat (the held letter keeps repeating, with the new modifier
      // state taking effect on the next tick via process_key).
      return;
    }
    held_key_ = event.key;
    is_held_ = true;
    arm();
    return;
  }

  // Release: only cancel if it's the key we were tracking.
  if (is_held_ && event.key == held_key_) {
    disarm();
    is_held_ = false;
    held_key_ = 0;
  }
}

void KeyRepeater::dispatch() const {
  if (timer_fd_ < 0) {
    return;
  }

  uint64_t expirations = 0;
  ssize_t const n = ::read(timer_fd_, &expirations, sizeof(expirations));
  if (n != static_cast<ssize_t>(sizeof(expirations))) {
    // EAGAIN (no expirations) or short read: nothing to do.
    return;
  }

  if (!is_held_ || handler_ == nullptr || keyboard_ == nullptr) {
    return;
  }

  // Cap synthesized events per dispatch so a stalled loop catching up
  // doesn't unleash a thousand-key burst on the consumer.
  constexpr uint64_t k_max_burst = 8;
  uint64_t const emit_count = expirations < k_max_burst ? expirations : k_max_burst;

  uint32_t const now_ms = monotonic_ms();
  for (uint64_t i = 0; i < emit_count; ++i) {
    // Re-check on every iteration: the handler invoked below can call
    // cancel() (or on_key() with a release) and clear is_held_, in
    // which case continuing to emit synthesized events for a key the
    // user has already released is a UX bug.
    if (!is_held_) {
      break;
    }
    KeyboardEvent ke{};
    ke.time_ms = now_ms;
    ke.key = held_key_;
    ke.pressed = true;
    ke.repeat = true;
    keyboard_->process_key(ke);
    handler_(ke);
  }
}

void KeyRepeater::cancel() {
  disarm();
  is_held_ = false;
  held_key_ = 0;
}

int KeyRepeater::fd() const noexcept {
  return timer_fd_;
}

uint32_t KeyRepeater::held_key() const noexcept {
  return is_held_ ? held_key_ : 0;
}

void KeyRepeater::arm() const {
  if (timer_fd_ < 0) {
    return;
  }
  itimerspec const spec = make_spec(cfg_.delay_ms, cfg_.interval_ms);
  ::timerfd_settime(timer_fd_, 0, &spec, nullptr);
}

void KeyRepeater::disarm() const {
  if (timer_fd_ < 0) {
    return;
  }
  constexpr itimerspec spec{};  // Both fields zero, equals disarmed.
  ::timerfd_settime(timer_fd_, 0, &spec, nullptr);
}

void KeyRepeater::close_timer_fd() {
  if (timer_fd_ >= 0) {
    ::close(timer_fd_);
    timer_fd_ = -1;
  }
}

}  // namespace drm::input