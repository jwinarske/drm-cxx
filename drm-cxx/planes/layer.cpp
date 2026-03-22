// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "layer.hpp"

namespace drm::planes {

Layer& Layer::set_property(std::string_view name, uint64_t value) {
  properties_[std::string(name)] = value;
  return *this;
}

Layer& Layer::disable() noexcept {
  properties_["FB_ID"] = 0;
  return *this;
}

Layer& Layer::set_composited() noexcept {
  force_composited_ = true;
  return *this;
}

bool Layer::needs_composition() const noexcept {
  return needs_composition_;
}

std::optional<uint32_t> Layer::assigned_plane_id() const noexcept {
  return assigned_plane_;
}

} // namespace drm::planes
