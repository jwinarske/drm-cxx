// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <vector>

#include "seat.hpp"

namespace drm::input {

class EventDispatcher {
public:
  void add_handler(EventHandler handler);
  void dispatch(const InputEvent& event);

private:
  std::vector<std::move_only_function<void(const InputEvent&)>> handlers_;
};

} // namespace drm::input
