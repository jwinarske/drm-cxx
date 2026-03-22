// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "plane_registry.hpp"

namespace drm::planes {

std::expected<PlaneRegistry, std::error_code>
PlaneRegistry::enumerate([[maybe_unused]] const Device& dev) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

std::span<const PlaneCapabilities> PlaneRegistry::all() const noexcept {
  return planes_;
}

std::span<const PlaneCapabilities>
PlaneRegistry::for_crtc([[maybe_unused]] uint32_t crtc_index) const {
  return planes_;
}

} // namespace drm::planes
