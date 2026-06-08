// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// display/scanout_target.cpp

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/scanout_target.hpp>
#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/modeset/mode.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <xf86drmMode.h>

#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>

namespace drm::display {

namespace {

std::error_code no_device() {
  return std::make_error_code(std::errc::no_such_device);
}

// Index of `crtc_id` in the resources CRTC table, or nullopt.
std::optional<std::uint32_t> crtc_index_of(const drmModeRes* res, std::uint32_t crtc_id) {
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtc_id) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::uint32_t> primary_plane_for_crtc(const planes::PlaneRegistry& planes,
                                                    std::uint32_t crtc_index) {
  for (const planes::PlaneCapabilities* cap : planes.for_crtc(crtc_index)) {
    if (cap->type == planes::DRMPlaneType::PRIMARY) {
      return cap->id;
    }
  }
  return std::nullopt;
}

drm::expected<ScanoutTarget, std::error_code> ScanoutTarget::discover(const drm::Device& dev) {
  const int fd = dev.fd();
  // The PRIMARY plane is only visible once universal planes are enabled.
  (void)dev.enable_universal_planes();

  drm::Resources res = drm::get_resources(fd);
  if (!res) {
    return drm::unexpected<std::error_code>(no_device());
  }

  // First connected connector that advertises modes.
  drm::Connector conn{nullptr, drmModeFreeConnector};
  for (int i = 0; i < res->count_connectors; ++i) {
    drm::Connector c = drm::get_connector(fd, res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      conn = std::move(c);
      break;
    }
  }
  if (!conn) {
    return drm::unexpected<std::error_code>(no_device());
  }

  ScanoutTarget t;
  t.connector_id = conn->connector_id;

  auto mode =
      drm::select_preferred_mode(drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes));
  if (!mode) {
    return drm::unexpected<std::error_code>(mode.error());
  }
  t.mode = mode->drm_mode;

  // Prefer the CRTC already bound to the connector; otherwise the first CRTC any
  // of its encoders allows.
  std::optional<std::uint32_t> crtc_index;
  if (conn->encoder_id != 0) {
    drm::Encoder enc = drm::get_encoder(fd, conn->encoder_id);
    if (enc && enc->crtc_id != 0) {
      crtc_index = crtc_index_of(res.get(), enc->crtc_id);
    }
  }
  for (int e = 0; !crtc_index && e < conn->count_encoders; ++e) {
    drm::Encoder enc = drm::get_encoder(fd, conn->encoders[e]);
    if (!enc) {
      continue;
    }
    for (int ci = 0; ci < res->count_crtcs; ++ci) {
      if ((enc->possible_crtcs & (1U << static_cast<unsigned>(ci))) != 0U) {
        crtc_index = static_cast<std::uint32_t>(ci);
        break;
      }
    }
  }
  if (!crtc_index) {
    return drm::unexpected<std::error_code>(no_device());
  }
  t.crtc_index = *crtc_index;
  t.crtc_id = res->crtcs[*crtc_index];

  auto registry = drm::planes::PlaneRegistry::enumerate(dev);
  if (!registry) {
    return drm::unexpected<std::error_code>(registry.error());
  }
  auto primary = primary_plane_for_crtc(*registry, t.crtc_index);
  if (!primary) {
    return drm::unexpected<std::error_code>(no_device());
  }
  t.primary_plane_id = *primary;

  // IN_FORMATS is advisory: attach it when present, otherwise leave it empty and
  // let the caller assume LINEAR (older/simple drivers expose no blob).
  if (auto formats = drm::fmt::FormatTable::from_plane(fd, t.primary_plane_id)) {
    t.primary_formats = std::move(*formats);
  }

  return t;
}

}  // namespace drm::display
