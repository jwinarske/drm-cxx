// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "seat.hpp"

namespace drm::input {

Seat::~Seat() = default;
Seat::Seat(Seat&&) noexcept = default;
Seat& Seat::operator=(Seat&&) noexcept = default;

std::expected<Seat, std::error_code>
Seat::open([[maybe_unused]] SeatOptions opts) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

void Seat::set_event_handler(EventHandler handler) {
  handler_ = std::move(handler);
}

std::expected<void, std::error_code> Seat::dispatch() {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace drm::input
