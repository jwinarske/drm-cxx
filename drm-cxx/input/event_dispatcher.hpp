// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "seat.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace drm::input {

// Fan-out dispatcher: routes InputEvents to multiple handlers.
class EventDispatcher {
 public:
  void add_handler(EventHandler handler);

  // Dispatch an event to all registered handlers.
  void dispatch(const InputEvent& event);

  // Returns a single EventHandler that feeds into this dispatcher.
  // Useful for Seat::set_event_handler(dispatcher.as_handler()).
  //
  // WARNING: The returned handler captures 'this'. The caller must ensure
  // that this EventDispatcher outlives the handler (e.g., the Seat it is
  // registered with). Using the handler after this dispatcher is destroyed
  // is undefined behavior.
  EventHandler as_handler();

  [[nodiscard]] std::size_t handler_count() const noexcept;

 private:
  std::vector<std::move_only_function<void(const InputEvent&)>> handlers_;
};

}  // namespace drm::input
