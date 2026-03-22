// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "output.hpp"

#include <algorithm>

namespace drm::planes {

Output::Output(uint32_t crtc_id, Layer& composition_layer)
  : crtc_id_(crtc_id), composition_layer_(&composition_layer) {}

Layer& Output::add_layer() {
  owned_layers_.push_back(std::make_unique<Layer>());
  return *owned_layers_.back();
}

void Output::remove_layer([[maybe_unused]] const Layer& layer) {
  std::erase_if(owned_layers_, [&](const auto& l) {
    return l.get() == &layer;
  });
}

void Output::set_composition_layer(Layer& layer) {
  composition_layer_ = &layer;
}

std::span<Layer*> Output::layers() noexcept {
  return {};
}

uint32_t Output::crtc_id() const noexcept {
  return crtc_id_;
}

} // namespace drm::planes
