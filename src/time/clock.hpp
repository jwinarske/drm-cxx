// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// time/clock.hpp — library-wide monotonic clock abstraction.
//
// Subsystems that animate, pace, or schedule (starting with
// drm::cursor::Renderer) take a `Clock&` so callers can:
//   - sync animation with an external presentation clock (a compositor
//     that already tracks vblank/present timestamps hands the same
//     time point to drm-cxx and to its own renderers);
//   - freeze or step time in tests without sleeping;
//   - consolidate on a single clock as other timing-aware subsystems
//     (page-flip pacing, input debouncing, lottie/thorvg cycles) land.
//
// The default implementation is std::chrono::steady_clock; callers who
// don't care get `drm::default_clock()` and never need to construct a
// Clock themselves.

#pragma once

#include <chrono>

namespace drm {

/// Monotonic clock interface. One virtual call per sample — negligible
/// next to a KMS ioctl, but if a hot path ever materializes the
/// interface can be swapped for a type-erased wrapper without an ABI
/// break (all callers go through Clock& / Clock*).
class Clock {
 public:
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::time_point<Clock, duration>;

  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  Clock(Clock&&) = delete;
  Clock& operator=(Clock&&) = delete;
  virtual ~Clock() = default;

  [[nodiscard]] virtual time_point now() const noexcept = 0;
};

/// Default implementation backed by std::chrono::steady_clock. Safe to
/// instantiate per caller or share via `default_clock()`; holds no
/// state.
class SteadyClock final : public Clock {
 public:
  [[nodiscard]] time_point now() const noexcept override;
};

/// Reference to a process-wide SteadyClock. The returned reference is
/// stable for the lifetime of the program.
[[nodiscard]] Clock& default_clock() noexcept;

}  // namespace drm