// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "layer.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace drm::planes {

class Output {
 public:
  Output(uint32_t crtc_id, Layer& composition_layer);

  Layer& add_layer();
  void remove_layer(const Layer& layer);

  void set_composition_layer(Layer& layer);

  [[nodiscard]] std::vector<Layer*>& layers() noexcept;
  [[nodiscard]] const std::vector<Layer*>& layers() const noexcept;
  [[nodiscard]] uint32_t crtc_id() const noexcept;
  [[nodiscard]] Layer* composition_layer() const noexcept;

  [[nodiscard]] bool any_layer_dirty() const noexcept;
  [[nodiscard]] std::vector<Layer*> changed_layers() const;
  void mark_clean() noexcept;

  void sort_layers_by_zpos();

 private:
  void rebuild_layer_ptrs();

  uint32_t crtc_id_;
  std::vector<std::unique_ptr<Layer>> owned_layers_;
  std::vector<Layer*> layer_ptrs_;
  Layer* composition_layer_{};
};

}  // namespace drm::planes
