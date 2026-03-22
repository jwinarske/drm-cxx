// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "surface.hpp"

namespace drm::gbm {

Surface::Surface(struct gbm_surface* surf) noexcept : surf_(surf) {}
Surface::~Surface() = default;

Surface::Surface(Surface&& other) noexcept : surf_(other.surf_) {
  other.surf_ = nullptr;
}

Surface& Surface::operator=(Surface&& other) noexcept {
  if (this != &other) {
    surf_ = other.surf_;
    other.surf_ = nullptr;
  }
  return *this;
}

std::expected<Surface, std::error_code>
Surface::create([[maybe_unused]] GbmDevice& dev,
                [[maybe_unused]] uint32_t width,
                [[maybe_unused]] uint32_t height,
                [[maybe_unused]] uint32_t format,
                [[maybe_unused]] uint32_t flags) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

struct gbm_surface* Surface::raw() const noexcept { return surf_; }

} // namespace drm::gbm
