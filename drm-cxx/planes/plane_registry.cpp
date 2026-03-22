// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "plane_registry.hpp"

#include "../core/device.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace drm::planes {

bool PlaneCapabilities::supports_format(uint32_t fmt) const {
  return std::ranges::find(formats, fmt) != formats.end();
}

bool PlaneCapabilities::compatible_with_crtc(uint32_t crtc_index) const {
  return (possible_crtcs & (1u << crtc_index)) != 0;
}

namespace {

DRMPlaneType parse_plane_type(int fd, uint32_t plane_id) {
  auto* props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (!props) return DRMPlaneType::OVERLAY;

  DRMPlaneType result = DRMPlaneType::OVERLAY;
  for (uint32_t i = 0; i < props->count_props; ++i) {
    auto* prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop) continue;

    if (std::strcmp(prop->name, "type") == 0) {
      auto val = props->prop_values[i];
      if (val == DRM_PLANE_TYPE_PRIMARY)
        result = DRMPlaneType::PRIMARY;
      else if (val == DRM_PLANE_TYPE_CURSOR)
        result = DRMPlaneType::CURSOR;
      else
        result = DRMPlaneType::OVERLAY;
      drmModeFreeProperty(prop);
      break;
    }
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);
  return result;
}

void detect_plane_capabilities(int fd, uint32_t plane_id, PlaneCapabilities& caps) {
  auto* props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (!props) return;

  for (uint32_t i = 0; i < props->count_props; ++i) {
    auto* prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop) continue;

    if (std::strcmp(prop->name, "zpos") == 0) {
      // For range properties, extract min/max
      if (prop->flags & DRM_MODE_PROP_RANGE && prop->count_values >= 2) {
        caps.zpos_min = prop->values[0];
        caps.zpos_max = prop->values[1];
      } else {
        caps.zpos_min = props->prop_values[i];
        caps.zpos_max = props->prop_values[i];
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

std::expected<PlaneRegistry, std::error_code> PlaneRegistry::enumerate(const Device& dev) {
  int fd = dev.fd();

  auto* plane_res = drmModeGetPlaneResources(fd);
  if (!plane_res) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }

  PlaneRegistry registry;
  registry.planes_.reserve(plane_res->count_planes);

  for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
    auto* plane = drmModeGetPlane(fd, plane_res->planes[i]);
    if (!plane) continue;

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

std::span<const PlaneCapabilities> PlaneRegistry::all() const noexcept {
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
