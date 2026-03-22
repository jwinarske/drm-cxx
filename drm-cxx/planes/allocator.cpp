// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "allocator.hpp"
#include "../modeset/atomic.hpp"

namespace drm::planes {

Allocator::Allocator(const Device& dev, PlaneRegistry& registry)
  : dev_(dev), registry_(registry) {}

std::expected<std::size_t, std::error_code>
Allocator::apply([[maybe_unused]] Output& output,
                 [[maybe_unused]] AtomicRequest& req,
                 [[maybe_unused]] uint32_t commit_flags) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

bool Allocator::try_assign([[maybe_unused]] Output& output,
                           [[maybe_unused]] std::span<Layer*> unassigned,
                           [[maybe_unused]] std::span<const PlaneCapabilities> available_planes,
                           [[maybe_unused]] AtomicRequest& req,
                           [[maybe_unused]] uint32_t flags) {
  return false;
}

bool Allocator::plane_compatible_with_layer(
    [[maybe_unused]] const PlaneCapabilities& plane,
    [[maybe_unused]] const Layer& layer,
    [[maybe_unused]] uint32_t crtc_index) const {
  return false;
}

} // namespace drm::planes
