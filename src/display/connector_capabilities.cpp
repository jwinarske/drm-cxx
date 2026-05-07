// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "connector_capabilities.hpp"

#include "../core/device.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <system_error>

namespace drm::display {

namespace {

void apply_colorspace_enum(ConnectorCapabilities& caps, const char* name, std::uint64_t value) {
  if (std::strcmp(name, "Default") == 0) {
    caps.colorspace_default = value;
  } else if (std::strcmp(name, "SMPTE_170M_YCC") == 0) {
    caps.colorspace_smpte170m_ycc = value;
  } else if (std::strcmp(name, "BT709_YCC") == 0) {
    caps.colorspace_bt709_ycc = value;
  } else if (std::strcmp(name, "XVYCC_601") == 0) {
    caps.colorspace_xvycc_601 = value;
  } else if (std::strcmp(name, "XVYCC_709") == 0) {
    caps.colorspace_xvycc_709 = value;
  } else if (std::strcmp(name, "SYCC_601") == 0) {
    caps.colorspace_sycc_601 = value;
  } else if (std::strcmp(name, "opYCC_601") == 0) {
    caps.colorspace_opycc_601 = value;
  } else if (std::strcmp(name, "opRGB") == 0) {
    caps.colorspace_oprgb = value;
  } else if (std::strcmp(name, "BT2020_CYCC") == 0) {
    caps.colorspace_bt2020_cycc = value;
  } else if (std::strcmp(name, "BT2020_RGB") == 0) {
    caps.colorspace_bt2020_rgb = value;
  } else if (std::strcmp(name, "BT2020_YCC") == 0) {
    caps.colorspace_bt2020_ycc = value;
  } else if (std::strcmp(name, "DCI-P3_RGB_D65") == 0) {
    caps.colorspace_dci_p3_rgb_d65 = value;
  } else if (std::strcmp(name, "DCI-P3_RGB_Theater") == 0) {
    caps.colorspace_dci_p3_rgb_theater = value;
  } else if (std::strcmp(name, "RGB_Wide_Gamut_Fixed_Point") == 0) {
    caps.colorspace_rgb_wide_fixed = value;
  } else if (std::strcmp(name, "RGB_Wide_Gamut_Floating_Point") == 0) {
    caps.colorspace_rgb_wide_float = value;
  }
}

void apply_broadcast_rgb_enum(ConnectorCapabilities& caps, const char* name, std::uint64_t value) {
  if (std::strcmp(name, "Automatic") == 0) {
    caps.broadcast_rgb_automatic = value;
  } else if (std::strcmp(name, "Full") == 0) {
    caps.broadcast_rgb_full = value;
  } else if (std::strcmp(name, "Limited 16:235") == 0) {
    caps.broadcast_rgb_limited = value;
  }
}

void parse_property(const drmModePropertyRes* prop, std::uint64_t value,
                    ConnectorCapabilities& caps) {
  if (std::strcmp(prop->name, "Colorspace") == 0) {
    caps.has_colorspace = true;
    if ((prop->flags & DRM_MODE_PROP_ENUM) != 0U) {
      const auto enums = drm::span<const drm_mode_property_enum>(prop->enums, prop->count_enums);
      for (const auto& en : enums) {
        apply_colorspace_enum(caps, en.name, en.value);
      }
    }
    return;
  }

  if (std::strcmp(prop->name, "max bpc") == 0) {
    caps.has_max_bpc = true;
    caps.max_bpc_current = value;
    if (((prop->flags & DRM_MODE_PROP_RANGE) != 0U) && prop->count_values >= 2) {
      const auto vals = drm::span<const std::uint64_t>(prop->values, prop->count_values);
      caps.max_bpc_min = vals[0];
      caps.max_bpc_max = vals[1];
    }
    return;
  }

  if (std::strcmp(prop->name, "HDR_OUTPUT_METADATA") == 0) {
    caps.has_hdr_output_metadata = true;
    return;
  }

  if (std::strcmp(prop->name, "Broadcast RGB") == 0) {
    caps.has_broadcast_rgb = true;
    if ((prop->flags & DRM_MODE_PROP_ENUM) != 0U) {
      const auto enums = drm::span<const drm_mode_property_enum>(prop->enums, prop->count_enums);
      for (const auto& en : enums) {
        apply_broadcast_rgb_enum(caps, en.name, en.value);
      }
    }
    return;
  }
}

}  // namespace

bool ConnectorCapabilities::can_signal_hdr() const noexcept {
  return has_hdr_output_metadata && has_max_bpc && max_bpc_max.has_value() && *max_bpc_max >= 10;
}

std::optional<std::uint64_t> ConnectorCapabilities::colorspace_value(
    const Colorspace cs) const noexcept {
  switch (cs) {
    case Colorspace::Default:
      return colorspace_default;
    case Colorspace::SmpteRf170mYcc:
      return colorspace_smpte170m_ycc;
    case Colorspace::Bt709Ycc:
      return colorspace_bt709_ycc;
    case Colorspace::XvYcc601:
      return colorspace_xvycc_601;
    case Colorspace::XvYcc709:
      return colorspace_xvycc_709;
    case Colorspace::SYcc601:
      return colorspace_sycc_601;
    case Colorspace::OpYcc601:
      return colorspace_opycc_601;
    case Colorspace::OpRgb:
      return colorspace_oprgb;
    case Colorspace::Bt2020Cycc:
      return colorspace_bt2020_cycc;
    case Colorspace::Bt2020Rgb:
      return colorspace_bt2020_rgb;
    case Colorspace::Bt2020Ycc:
      return colorspace_bt2020_ycc;
    case Colorspace::DciP3RgbD65:
      return colorspace_dci_p3_rgb_d65;
    case Colorspace::DciP3RgbTheater:
      return colorspace_dci_p3_rgb_theater;
    case Colorspace::RgbWideFixed:
      return colorspace_rgb_wide_fixed;
    case Colorspace::RgbWideFloat:
      return colorspace_rgb_wide_float;
  }
  return std::nullopt;
}

std::optional<std::uint64_t> ConnectorCapabilities::broadcast_rgb_value(
    const BroadcastRgb b) const noexcept {
  switch (b) {
    case BroadcastRgb::Automatic:
      return broadcast_rgb_automatic;
    case BroadcastRgb::Full:
      return broadcast_rgb_full;
    case BroadcastRgb::Limited:
      return broadcast_rgb_limited;
  }
  return std::nullopt;
}

drm::expected<ConnectorCapabilities, std::error_code> probe_connector_capabilities(
    const drm::Device& dev, const std::uint32_t connector_id) {
  auto* props = drmModeObjectGetProperties(dev.fd(), connector_id, DRM_MODE_OBJECT_CONNECTOR);
  if (props == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  ConnectorCapabilities caps;
  caps.connector_id = connector_id;

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