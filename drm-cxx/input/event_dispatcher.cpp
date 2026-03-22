// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "event_dispatcher.hpp"

namespace drm::input {

void EventDispatcher::add_handler(EventHandler handler) {
  handlers_.push_back(std::move(handler));
}

void EventDispatcher::dispatch(const InputEvent& event) {
  for (auto& handler : handlers_) {
    handler(event);
  }
}

EventHandler EventDispatcher::as_handler() {
  return [this](const InputEvent& event) { dispatch(event); };
}

std::size_t EventDispatcher::handler_count() const noexcept {
  return handlers_.size();
}

}  // namespace drm::input
