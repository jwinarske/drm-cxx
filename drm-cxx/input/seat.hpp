// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <functional>
#include <string_view>
#include <system_error>
#include <variant>

namespace drm::input {

struct KeyboardEvent {};
struct PointerEvent {};
struct TouchEvent {};
struct SwitchEvent {};

using InputEvent = std::variant<KeyboardEvent, PointerEvent, TouchEvent, SwitchEvent>;
using EventHandler = std::move_only_function<void(const InputEvent&)>;

struct SeatOptions {
  std::string_view seat_name = "seat0";
};

class Seat {
public:
  static std::expected<Seat, std::error_code>
    open(SeatOptions opts = {});

  void set_event_handler(EventHandler handler);

  std::expected<void, std::error_code> dispatch();

  ~Seat();
  Seat(Seat&&) noexcept;
  Seat& operator=(Seat&&) noexcept;
  Seat(const Seat&) = delete;
  Seat& operator=(const Seat&) = delete;

private:
  Seat() = default;
  EventHandler handler_;
};

} // namespace drm::input
