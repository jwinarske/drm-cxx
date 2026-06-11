// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "mode_list.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/modeset/mode.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::display {

const char* connector_type_name(std::uint32_t connector_type) noexcept {
  switch (connector_type) {
    case DRM_MODE_CONNECTOR_VGA:
      return "VGA";
    case DRM_MODE_CONNECTOR_DVII:
      return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:
      return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:
      return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:
      return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO:
      return "S-Video";
    case DRM_MODE_CONNECTOR_LVDS:
      return "LVDS";
    case DRM_MODE_CONNECTOR_Component:
      return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN:
      return "DIN";
    case DRM_MODE_CONNECTOR_DisplayPort:
      return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:
      return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:
      return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:
      return "TV";
    case DRM_MODE_CONNECTOR_eDP:
      return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:
      return "Virtual";
    case DRM_MODE_CONNECTOR_DSI:
      return "DSI";
    case DRM_MODE_CONNECTOR_DPI:
      return "DPI";
    case DRM_MODE_CONNECTOR_WRITEBACK:
      return "Writeback";
    case DRM_MODE_CONNECTOR_SPI:
      return "SPI";
    case DRM_MODE_CONNECTOR_USB:
      return "USB";
    default:
      return "Unknown";
  }
}

std::string ConnectorModes::name() const {
  return std::string(connector_type_name(connector_type)) + "-" + std::to_string(connector_type_id);
}

drm::expected<std::vector<ConnectorModes>, std::error_code> query_connector_modes(
    drm::Device& dev) {
  auto res = drm::get_resources(dev.fd());
  if (!res) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::generic_category()));
  }
  std::vector<ConnectorModes> out;
  out.reserve(static_cast<std::size_t>(res->count_connectors));
  for (int i = 0; i < res->count_connectors; ++i) {
    auto conn = drm::get_connector(dev.fd(), res->connectors[i]);
    if (!conn) {
      continue;  // a connector can vanish mid-enumeration (hotplug); skip it
    }
    ConnectorModes c;
    c.connector_id = conn->connector_id;
    c.connector_type = conn->connector_type;
    c.connector_type_id = conn->connector_type_id;
    c.connected = conn->connection == DRM_MODE_CONNECTED;
    c.modes.reserve(static_cast<std::size_t>(conn->count_modes));
    for (int m = 0; m < conn->count_modes; ++m) {
      c.modes.push_back(drm::ModeInfo{conn->modes[m]});
    }
    out.push_back(std::move(c));
  }
  return out;
}

}  // namespace drm::display
