// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "scanout_format.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm_fourcc.h>

#include <array>
#include <cstdint>
#include <optional>

namespace drm::present {

namespace {

// Default order: 32-bpp packed first (most color depth), 16-bpp last.
constexpr std::array<std::uint32_t, 6> k_default_prefs{
    DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888,
    DRM_FORMAT_ABGR8888, DRM_FORMAT_RGB565,   DRM_FORMAT_BGR565,
};

std::optional<std::uint32_t> crtc_index_for(const drm::Device& dev, std::uint32_t crtc_id) {
  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    return std::nullopt;
  }
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtc_id) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return std::nullopt;
}

std::uint32_t pick(const drm::planes::PlaneCapabilities* plane,
                   drm::span<const std::uint32_t> prefs) {
  if (plane == nullptr) {
    return 0;
  }
  for (const std::uint32_t fourcc : prefs) {
    if (plane->supports_format(fourcc)) {
      return fourcc;
    }
  }
  return 0;
}

}  // namespace

std::uint32_t negotiate_scanout_format(const drm::Device& dev, std::uint32_t crtc_id,
                                       drm::span<const std::uint32_t> preferred) {
  auto registry = drm::planes::PlaneRegistry::enumerate(dev);
  if (!registry) {
    return 0;
  }
  const auto idx = crtc_index_for(dev, crtc_id);
  if (!idx) {
    return 0;
  }
  const auto prefs =
      preferred.empty() ? drm::span<const std::uint32_t>(k_default_prefs) : preferred;

  const auto& planes = registry->for_crtc(*idx);
  // Prefer the PRIMARY plane (where a full-screen scanout lands); fall back to
  // any compatible plane.
  const drm::planes::PlaneCapabilities* primary = nullptr;
  for (const auto* plane : planes) {
    if (plane->type == drm::planes::DRMPlaneType::PRIMARY) {
      primary = plane;
      break;
    }
  }
  if (const std::uint32_t fourcc = pick(primary, prefs); fourcc != 0) {
    return fourcc;
  }
  for (const auto* plane : planes) {
    if (const std::uint32_t fourcc = pick(plane, prefs); fourcc != 0) {
      return fourcc;
    }
  }
  return 0;
}

}  // namespace drm::present
