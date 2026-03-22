// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "output.hpp"

#include "planes/layer.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace drm::planes {

Output::Output(uint32_t crtc_id, Layer& composition_layer)
    : crtc_id_(crtc_id), composition_layer_(&composition_layer) {
  composition_layer.is_composition_layer_ = true;
}

Layer& Output::add_layer() {
  owned_layers_.push_back(std::make_unique<Layer>());
  auto* ptr = owned_layers_.back().get();
  layer_ptrs_.push_back(ptr);
  return *ptr;
}

void Output::remove_layer(const Layer& layer) {
  std::erase_if(layer_ptrs_, [&](const Layer* l) { return l == &layer; });
  std::erase_if(owned_layers_, [&](const auto& l) { return l.get() == &layer; });
}

void Output::set_composition_layer(Layer& layer) {
  if (composition_layer_ != nullptr) {
    composition_layer_->is_composition_layer_ = false;
  }
  composition_layer_ = &layer;
  layer.is_composition_layer_ = true;
}

std::vector<Layer*>& Output::layers() noexcept {
  return layer_ptrs_;
}

const std::vector<Layer*>& Output::layers() const noexcept {
  return layer_ptrs_;
}

uint32_t Output::crtc_id() const noexcept {
  return crtc_id_;
}

Layer* Output::composition_layer() const noexcept {
  return composition_layer_;
}

bool Output::any_layer_dirty() const noexcept {
  for (const auto* layer : layer_ptrs_) {
    if (layer->is_dirty()) {
      return true;
    }
  }
  return composition_layer_ && composition_layer_->is_dirty();
}

std::vector<Layer*> Output::changed_layers() const {
  std::vector<Layer*> result;
  for (auto* layer : layer_ptrs_) {
    if (layer->is_dirty()) {
      result.push_back(layer);
    }
  }
  return result;
}

void Output::mark_clean() noexcept {
  for (auto* layer : layer_ptrs_) {
    layer->mark_clean();
  }
  if (composition_layer_ != nullptr) {
    composition_layer_->mark_clean();
  }
}

void Output::sort_layers_by_zpos() {
  std::ranges::sort(layer_ptrs_, [](const Layer* a, const Layer* b) {
    auto za = a->property("zpos").value_or(0);
    auto zb = b->property("zpos").value_or(0);
    return za < zb;
  });
}

void Output::rebuild_layer_ptrs() {
  layer_ptrs_.clear();
  for (const auto& l : owned_layers_) {
    layer_ptrs_.push_back(l.get());
  }
}

}  // namespace drm::planes
