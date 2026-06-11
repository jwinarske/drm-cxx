// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// mode_list.hpp — connector mode enumeration for mode listing / selection.
//
// Reads every connector's advertised modes via libdrm only (no GL/Vulkan/GBM),
// so it is safe to call before any backend is brought up — e.g. to implement a
// `--list-modes` diagnostic or to pick a startup mode. Pairs with
// `drm::planes::PlaneRegistry` (which already exposes per-plane rotation
// capability) for a full device listing.

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/modeset/mode.hpp>

#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::display {

/// Friendly connector-type name (e.g. "HDMI-A", "eDP", "DP"). libdrm's
/// `drmModeGetConnectorTypeName` can return nullptr on older libdrm, so this
/// keeps a local table; it never returns nullptr (unknown types map to
/// "Unknown").
[[nodiscard]] const char* connector_type_name(std::uint32_t connector_type) noexcept;

/// One connector and the modes it advertises.
struct ConnectorModes {
  std::uint32_t connector_id{};
  std::uint32_t connector_type{};     ///< DRM_MODE_CONNECTOR_*
  std::uint32_t connector_type_id{};  ///< per-type instance index (the "-N" suffix)
  bool connected{false};
  std::vector<drm::ModeInfo> modes;

  /// Connector name in the conventional "HDMI-A-1" / "eDP-1" form.
  [[nodiscard]] std::string name() const;
};

/// Enumerate every connector on the device together with its advertised modes.
/// Returns connectors in resource order (matching `drmModeGetResources`).
[[nodiscard]] drm::expected<std::vector<ConnectorModes>, std::error_code> query_connector_modes(
    drm::Device& dev);

}  // namespace drm::display
