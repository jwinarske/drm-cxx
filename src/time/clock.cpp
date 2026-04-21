// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "clock.hpp"

#include <chrono>

namespace drm {

Clock::time_point SteadyClock::now() const noexcept {
  // steady_clock::time_point has its own epoch tag, so we project it onto
  // the Clock tag by lifting the duration. Animation only cares about
  // intervals, not absolute epoch.
  const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
  return Clock::time_point(std::chrono::duration_cast<Clock::duration>(since_epoch));
}

Clock& default_clock() noexcept {
  static SteadyClock k_clock;
  return k_clock;
}

}  // namespace drm
