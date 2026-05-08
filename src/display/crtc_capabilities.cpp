// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "crtc_capabilities.hpp"

#include "../core/device.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <system_error>

namespace drm::display {

namespace {

void parse_property(const drmModePropertyRes* prop, std::uint64_t value, CrtcCapabilities& caps) {
  if (std::strcmp(prop->name, "DEGAMMA_LUT") == 0) {
    caps.has_degamma_lut = true;
    return;
  }
  if (std::strcmp(prop->name, "GAMMA_LUT") == 0) {
    caps.has_gamma_lut = true;
    return;
  }
  if (std::strcmp(prop->name, "CTM") == 0) {
    caps.has_ctm = true;
    return;
  }
  // Immutable LUT-size advertisements live as range properties.
  // The upper bound (values[1]) is the LUT row count the driver
  // expects in the blob; values[0] is typically 0. Some drivers
  // model these as a single-value range; treat values[0] as the
  // size in that case.
  if (std::strcmp(prop->name, "DEGAMMA_LUT_SIZE") == 0) {
    if (((prop->flags & DRM_MODE_PROP_RANGE) != 0U) && prop->count_values >= 1) {
      caps.degamma_lut_size = static_cast<std::uint32_t>(value);
    }
    return;
  }
  if (std::strcmp(prop->name, "GAMMA_LUT_SIZE") == 0) {
    if (((prop->flags & DRM_MODE_PROP_RANGE) != 0U) && prop->count_values >= 1) {
      caps.gamma_lut_size = static_cast<std::uint32_t>(value);
    }
    return;
  }
}

}  // namespace

drm::expected<CrtcCapabilities, std::error_code> probe_crtc_capabilities(
    const drm::Device& dev, const std::uint32_t crtc_id) {
  auto* props = drmModeObjectGetProperties(dev.fd(), crtc_id, DRM_MODE_OBJECT_CRTC);
  if (props == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  CrtcCapabilities caps;
  caps.crtc_id = crtc_id;

  const auto prop_ids = drm::span<const std::uint32_t>(props->props, props->count_props);
  const auto prop_vals = drm::span<const std::uint64_t>(props->prop_values, props->count_props);

  for (std::uint32_t i = 0; i < props->count_props; ++i) {
    auto* prop = drmModeGetProperty(dev.fd(), prop_ids[i]);
    if (prop == nullptr) {
      continue;
    }
    parse_property(prop, prop_vals[i], caps);
    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
  return caps;
}

}  // namespace drm::display
