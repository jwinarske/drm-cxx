// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "layer.hpp"

namespace drm::planes {

class Output {
public:
  Output(uint32_t crtc_id, Layer& composition_layer);

  Layer& add_layer();
  void remove_layer(const Layer& layer);

  void set_composition_layer(Layer& layer);

  [[nodiscard]] std::span<Layer*> layers() noexcept;
  [[nodiscard]] uint32_t crtc_id() const noexcept;

private:
  uint32_t crtc_id_;
  std::vector<std::unique_ptr<Layer>> owned_layers_;
  Layer* composition_layer_{};
};

} // namespace drm::planes
