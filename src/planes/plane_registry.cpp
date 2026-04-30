// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "plane_registry.hpp"

#include "../core/device.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_fourcc.h>
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

bool PlaneCapabilities::supports_format_modifier(const uint32_t fmt,
                                                 const uint64_t modifier) const {
  // INVALID is the legacy sentinel for "implementation-defined", which
  // every driver lacking IN_FORMATS treats as linear. Normalize it so
  // both code paths compare against a single LINEAR identity.
  const bool is_linear = modifier == DRM_FORMAT_MOD_LINEAR || modifier == DRM_FORMAT_MOD_INVALID;

  if (!has_format_modifiers) {
    // No IN_FORMATS — accept LINEAR/INVALID against the bare format list,
    // reject non-trivial modifiers (AFBC, DCC, tilings) outright. We have
    // no evidence the plane can scan them out.
    return is_linear && supports_format(fmt);
  }

  // format_modifiers is sorted by format ascending (parse_in_formats_blob
  // sorts at the end of the parse). Bisect to the per-format slice in
  // O(log N), then walk the (typically <10) modifiers within it. The
  // allocator's bipartite/backtrack search hits this on every (plane,
  // layer) pair — score_pair calls it once per ranking comparison and
  // plane_statically_compatible once per static screening — so the
  // hot-path cost matters even at modest blob sizes.
  const auto cmp_format = [](const std::pair<uint32_t, uint64_t>& entry, uint32_t f) {
    return entry.first < f;
  };
  const auto first =
      std::lower_bound(format_modifiers.begin(), format_modifiers.end(), fmt, cmp_format);
  for (auto it = first; it != format_modifiers.end() && it->first == fmt; ++it) {
    if (it->second == modifier) {
      return true;
    }
    // Treat LINEAR and INVALID as interchangeable on either side of the
    // comparison: a layer tagged INVALID matches a plane entry of LINEAR
    // and vice versa.
    if (is_linear &&
        (it->second == DRM_FORMAT_MOD_LINEAR || it->second == DRM_FORMAT_MOD_INVALID)) {
      return true;
    }
  }
  return false;
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

void parse_in_formats_blob(const int fd, const uint32_t blob_id, PlaneCapabilities& caps) {
  auto* blob = drmModeGetPropertyBlob(fd, blob_id);
  if (blob == nullptr) {
    return;
  }

  drmModeFormatModifierIterator iter{};
  caps.format_modifiers.clear();
  while (drmModeFormatModifierBlobIterNext(blob, &iter)) {
    caps.format_modifiers.emplace_back(iter.fmt, iter.mod);
  }
  caps.has_format_modifiers = !caps.format_modifiers.empty();

  // Sort by format first, then by modifier — supports_format_modifier()
  // lower_bounds on format and walks the per-format slice. Sorting once
  // here turns every per-(plane, layer) eligibility check into
  // O(log N + K) instead of O(N).
  std::sort(caps.format_modifiers.begin(), caps.format_modifiers.end());

  drmModeFreePropertyBlob(blob);
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
    } else if (std::strcmp(prop->name, "IN_FORMATS") == 0) {
      // The property value is a blob id; the blob carries (format,
      // modifier) pairs the plane can scan out. Drivers that don't
      // expose IN_FORMATS (older kernels, some embedded stacks) leave
      // has_format_modifiers false and the format-only fallback path in
      // supports_format_modifier handles them.
      parse_in_formats_blob(fd, static_cast<uint32_t>(prop_vals[i]), caps);
    } else if (std::strcmp(prop->name, "pixel blend mode") == 0) {
      // Enum property: kernel exposes blend modes as named integers.
      // Each driver registers its own ordering, so the integer for
      // "Pre-multiplied" varies — cache the per-plane mapping here so
      // arm_composition_canvas can write the right value.
      caps.has_pixel_blend_mode = true;
      if ((prop->flags & DRM_MODE_PROP_ENUM) != 0U) {
        const auto enums = drm::span<const drm_mode_property_enum>(prop->enums, prop->count_enums);
        for (const auto& en : enums) {
          if (std::strcmp(en.name, "Pre-multiplied") == 0) {
            caps.blend_mode_premultiplied = en.value;
          } else if (std::strcmp(en.name, "Coverage") == 0) {
            caps.blend_mode_coverage = en.value;
          } else if (std::strcmp(en.name, "None") == 0) {
            caps.blend_mode_none = en.value;
          }
        }
      }
    } else if (std::strcmp(prop->name, "alpha") == 0) {
      caps.has_per_plane_alpha = true;
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

const std::vector<const PlaneCapabilities*>& PlaneRegistry::for_crtc(uint32_t crtc_index) const {
  if (auto it = for_crtc_cache_.find(crtc_index); it != for_crtc_cache_.end()) {
    return it->second;
  }
  std::vector<const PlaneCapabilities*> result;
  for (const auto& p : planes_) {
    if (p.compatible_with_crtc(crtc_index)) {
      result.push_back(&p);
    }
  }
  return for_crtc_cache_.emplace(crtc_index, std::move(result)).first->second;
}

}  // namespace drm::planes
