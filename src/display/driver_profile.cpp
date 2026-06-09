// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
// display/driver_profile.cpp

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/display/driver_profile.hpp>

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <system_error>

namespace drm::display {

bool connector_type_self_refreshes(std::uint32_t connector_type) noexcept {
  // Only embedded panels (eDP) and command-mode DSI can hold their own image
  // without a flip. Everything external (HDMI/DP/VGA/...) cannot.
  return connector_type == DRM_MODE_CONNECTOR_eDP || connector_type == DRM_MODE_CONNECTOR_DSI;
}

namespace {

// True iff object `id` of `type` exposes a property named `name`.
[[nodiscard]] bool object_has_property(int fd, std::uint32_t id, std::uint32_t type,
                                       const char* name) {
  bool found = false;
  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, id, type);
  if (props != nullptr) {
    for (std::uint32_t i = 0; i < props->count_props && !found; ++i) {
      if (drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i])) {
        found = std::strcmp(p->name, name) == 0;
        drmModeFreeProperty(p);
      }
    }
    drmModeFreeObjectProperties(props);
  }
  return found;
}

// Enumerate planes / CRTCs / connectors for the per-object frame-economy caps.
// Best-effort: any failed query leaves the cap at its conservative default.
void probe_object_caps(int fd, DriverProfile& profile) {
  // Universal-planes makes PRIMARY/CURSOR planes (not just OVERLAY) visible, so
  // the FB_DAMAGE_CLIPS scan sees the whole plane set. Atomic exposes the modern
  // property set. Both are client-side toggles; failure degrades gracefully.
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

  if (drmModePlaneResPtr planes = drmModeGetPlaneResources(fd)) {
    for (std::uint32_t i = 0; i < planes->count_planes && !profile.fb_damage_clips; ++i) {
      profile.fb_damage_clips =
          object_has_property(fd, planes->planes[i], DRM_MODE_OBJECT_PLANE, "FB_DAMAGE_CLIPS");
    }
    drmModeFreePlaneResources(planes);
  }

  drmModeResPtr res = drmModeGetResources(fd);
  if (res == nullptr) {
    return;
  }
  for (int i = 0; i < res->count_crtcs && !profile.vrr_capable; ++i) {
    profile.vrr_capable = object_has_property(fd, static_cast<std::uint32_t>(res->crtcs[i]),
                                              DRM_MODE_OBJECT_CRTC, "VRR_ENABLED");
  }

  // PSR telemetry: a connected eDP/DSI panel may self-refresh; external can't.
  bool any_connected = false;
  bool self_refresh = false;
  for (int i = 0; i < res->count_connectors; ++i) {
    // ...Current(): read cached connector state, don't force a (slow) re-probe.
    drmModeConnectorPtr conn =
        drmModeGetConnectorCurrent(fd, static_cast<std::uint32_t>(res->connectors[i]));
    if (conn == nullptr) {
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED) {
      any_connected = true;
      if (connector_type_self_refreshes(conn->connector_type)) {
        self_refresh = true;
      }
    }
    drmModeFreeConnector(conn);
  }
  if (self_refresh) {
    profile.psr = PanelSelfRefresh::Possible;
  } else if (any_connected) {
    profile.psr = PanelSelfRefresh::None;
  }
  drmModeFreeResources(res);
}

}  // namespace

PrimeCaps decode_prime_caps(std::uint64_t cap) noexcept {
  return PrimeCaps{(cap & DRM_PRIME_CAP_IMPORT) != 0U, (cap & DRM_PRIME_CAP_EXPORT) != 0U};
}

namespace {

bool cap_flag(int fd, std::uint64_t cap) {
  std::uint64_t value = 0;
  return drmGetCap(fd, cap, &value) == 0 && value != 0U;
}

std::uint64_t cap_value(int fd, std::uint64_t cap, std::uint64_t fallback) {
  std::uint64_t value = 0;
  return (drmGetCap(fd, cap, &value) == 0 && value != 0U) ? value : fallback;
}

}  // namespace

drm::expected<DriverProfile, std::error_code> DriverProfile::probe(const drm::Device& dev) {
  const int fd = dev.fd();

  DriverProfile profile;
  if (drmVersionPtr version = drmGetVersion(fd)) {
    if (version->name != nullptr) {
      profile.name.assign(version->name, static_cast<std::size_t>(version->name_len));
    }
    drmFreeVersion(version);
  } else {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }

  profile.addfb2_modifiers = cap_flag(fd, DRM_CAP_ADDFB2_MODIFIERS);
  profile.async_page_flip = cap_flag(fd, DRM_CAP_ASYNC_PAGE_FLIP);
  profile.timestamp_monotonic = cap_flag(fd, DRM_CAP_TIMESTAMP_MONOTONIC);

  std::uint64_t prime = 0;
  if (drmGetCap(fd, DRM_CAP_PRIME, &prime) == 0) {
    const PrimeCaps pc = decode_prime_caps(prime);
    profile.prime_import = pc.can_import;
    profile.prime_export = pc.can_export;
  }

  profile.cursor_width = cap_value(fd, DRM_CAP_CURSOR_WIDTH, 64);
  profile.cursor_height = cap_value(fd, DRM_CAP_CURSOR_HEIGHT, 64);

  probe_object_caps(fd, profile);

  return profile;
}

}  // namespace drm::display
