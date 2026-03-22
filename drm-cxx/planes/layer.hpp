// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace drm::planes {

class Allocator;

class Layer {
public:
  Layer& set_property(std::string_view name, uint64_t value);
  Layer& disable() noexcept;
  Layer& set_composited() noexcept;

  [[nodiscard]] bool needs_composition() const noexcept;
  [[nodiscard]] std::optional<uint32_t> assigned_plane_id() const noexcept;

private:
  friend class Allocator;
  std::unordered_map<std::string, uint64_t> properties_;
  bool force_composited_{false};
  bool needs_composition_{false};
  std::optional<uint32_t> assigned_plane_;
};

} // namespace drm::planes
