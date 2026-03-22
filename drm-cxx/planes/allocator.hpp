// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

#include "layer.hpp"
#include "output.hpp"
#include "plane_registry.hpp"
#include "../core/property_store.hpp"

namespace drm {
class Device;
class AtomicRequest;
} // namespace drm

namespace drm::planes {

class Allocator {
public:
  Allocator(const Device& dev, PlaneRegistry& registry);

  std::expected<std::size_t, std::error_code>
    apply(Output& output, AtomicRequest& req, uint32_t commit_flags);

private:
  bool try_assign(Output& output,
                  std::span<Layer*> unassigned,
                  std::span<const PlaneCapabilities> available_planes,
                  AtomicRequest& req,
                  uint32_t flags);

  bool plane_compatible_with_layer(const PlaneCapabilities& plane,
                                   const Layer& layer,
                                   uint32_t crtc_index) const;

  const Device& dev_;
  PlaneRegistry& registry_;
  PropertyStore prop_store_;
};

} // namespace drm::planes
