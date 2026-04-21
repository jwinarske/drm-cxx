// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "plane_registry.hpp"

#include "../core/device.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::planes {

bool PlaneCapabilities::supports_format(const uint32_t fmt) const {
  return std::find(formats.begin(), formats.end(), fmt) != formats.end();
}

bool PlaneCapabilities::compatible_with_crtc(const uint32_t crtc_index) const {
  return (possible_crtcs & (1U << crtc_index)) != 0;
}

namespace {

DRMPlaneType parse_plane_type(const int fd, const uint32_t plane_id) {
  auto* props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (props == nullptr) {
    return DRMPlaneType::OVERLAY;
  }

  const auto prop_ids = drm::span<const uint32_t>(props->props, props->count_props);
  const auto prop_vals = drm::span<const uint64_t>(props->prop_values, props->count_props);

  DRMPlaneType result = DRMPlaneType::OVERLAY;
  for (uint32_t i = 0; i < props->count_props; ++i) {
    auto* prop = drmModeGetProperty(fd, prop_ids[i]);
    if (prop == nullptr) {
      continue;
    }

    if (std::strcmp(prop->name, "type") == 0) {
      if (const auto val = prop_vals[i]; val == DRM_PLANE_TYPE_PRIMARY) {
        result = DRMPlaneType::PRIMARY;
      } else if (val == DRM_PLANE_TYPE_CURSOR) {
        result = DRMPlaneType::CURSOR;
      } else {
        result = DRMPlaneType::OVERLAY;
      }
      drmModeFreeProperty(prop);
      break;
    }
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);
  return result;
}

void detect_plane_capabilities(const int fd, const uint32_t plane_id, PlaneCapabilities& caps) {
  auto* props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (props == nullptr) {
    return;
  }

  const auto prop_ids = drm::span<const uint32_t>(props->props, props->count_props);
  const auto prop_vals = drm::span<const uint64_t>(props->prop_values, props->count_props);

  for (uint32_t i = 0; i < props->count_props; ++i) {
    auto* prop = drmModeGetProperty(fd, prop_ids[i]);
    if (prop == nullptr) {
      continue;
    }

    if (std::strcmp(prop->name, "zpos") == 0) {
      // For range properties, extract min/max
      if (((prop->flags & DRM_MODE_PROP_RANGE) != 0U) && prop->count_values >= 2) {
        const auto prop_values = drm::span<const uint64_t>(prop->values, prop->count_values);
        caps.zpos_min = prop_values[0];
        caps.zpos_max = prop_values[1];
      } else {
        caps.zpos_min = prop_vals[i];
        caps.zpos_max = prop_vals[i];
      }
    } else if (std::strcmp(prop->name, "rotation") == 0) {
      caps.supports_rotation = true;
    } else if (std::strcmp(prop->name, "SRC_W") == 0) {
      // If SRC_W exists and is different from CRTC_W range, scaling is supported
      caps.supports_scaling = true;
    }

    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);
}

}  // namespace

drm::expected<PlaneRegistry, std::error_code> PlaneRegistry::enumerate(const Device& dev) {
  int const fd = dev.fd();

  auto* plane_res = drmModeGetPlaneResources(fd);
  if (plane_res == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  PlaneRegistry registry;
  registry.planes_.reserve(plane_res->count_planes);

  const auto plane_ids = drm::span<const uint32_t>(plane_res->planes, plane_res->count_planes);

  for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
    auto* plane = drmModeGetPlane(fd, plane_ids[i]);
    if (plane == nullptr) {
      continue;
    }

    PlaneCapabilities caps;
    caps.id = plane->plane_id;
    caps.possible_crtcs = plane->possible_crtcs;
    caps.formats.assign(plane->formats, plane->formats + plane->count_formats);
    caps.type = parse_plane_type(fd, plane->plane_id);

    detect_plane_capabilities(fd, plane->plane_id, caps);

    drmModeFreePlane(plane);
    registry.planes_.push_back(std::move(caps));
  }

  drmModeFreePlaneResources(plane_res);
  return registry;
}

drm::span<const PlaneCapabilities> PlaneRegistry::all() const noexcept {
  return planes_;
}

std::vector<const PlaneCapabilities*> PlaneRegistry::for_crtc(uint32_t crtc_index) const {
  std::vector<const PlaneCapabilities*> result;
  for (const auto& p : planes_) {
    if (p.compatible_with_crtc(crtc_index)) {
      result.push_back(&p);
    }
  }
  return result;
}

}  // namespace drm::planes
