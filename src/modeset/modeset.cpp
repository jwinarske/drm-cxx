// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "modeset/modeset.hpp"

#include "core/device.hpp"
#include "modeset/atomic.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <system_error>

namespace drm::modeset {
namespace {

// Find the DRM property id of `name` on an object of the given type.
// Returns 0 if not found.
uint32_t find_property_id(const int fd, const uint32_t obj_id, const uint32_t obj_type,
                          const char* name) {
  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
  if (props == nullptr) {
    return 0;
  }
  uint32_t found = 0;
  for (uint32_t i = 0; i < props->count_props && found == 0; ++i) {
    drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
    if (p == nullptr) {
      continue;
    }
    if (std::strcmp(p->name, name) == 0) {
      found = p->prop_id;
    }
    drmModeFreeProperty(p);
  }
  drmModeFreeObjectProperties(props);
  return found;
}

}  // namespace

// ── Factory ──────────────────────────────────────────────────────────────

drm::expected<Modeset, std::error_code> Modeset::create(const Device& dev, const uint32_t crtc_id,
                                                        const uint32_t connector_id,
                                                        const drmModeModeInfo& mode) {
  Modeset m;
  m.fd_ = dev.fd();
  m.crtc_id_ = crtc_id;
  m.connector_id_ = connector_id;

  m.prop_mode_id_ = find_property_id(m.fd_, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
  m.prop_active_ = find_property_id(m.fd_, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
  m.prop_connector_crtc_ =
      find_property_id(m.fd_, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

  if (m.prop_mode_id_ == 0 || m.prop_active_ == 0 || m.prop_connector_crtc_ == 0) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::function_not_supported));
  }

  drmModeModeInfo mode_copy = mode;
  if (drmModeCreatePropertyBlob(m.fd_, &mode_copy, sizeof(mode_copy), &m.mode_blob_id_) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  return m;
}

// ── Lifecycle ────────────────────────────────────────────────────────────

Modeset::~Modeset() {
  release();
}

Modeset::Modeset(Modeset&& other) noexcept
    : fd_(other.fd_),
      crtc_id_(other.crtc_id_),
      connector_id_(other.connector_id_),
      prop_mode_id_(other.prop_mode_id_),
      prop_active_(other.prop_active_),
      prop_connector_crtc_(other.prop_connector_crtc_),
      mode_blob_id_(other.mode_blob_id_) {
  other.fd_ = -1;
  other.mode_blob_id_ = 0;
}

Modeset& Modeset::operator=(Modeset&& other) noexcept {
  if (this != &other) {
    release();
    fd_ = other.fd_;
    crtc_id_ = other.crtc_id_;
    connector_id_ = other.connector_id_;
    prop_mode_id_ = other.prop_mode_id_;
    prop_active_ = other.prop_active_;
    prop_connector_crtc_ = other.prop_connector_crtc_;
    mode_blob_id_ = other.mode_blob_id_;
    other.fd_ = -1;
    other.mode_blob_id_ = 0;
  }
  return *this;
}

void Modeset::release() noexcept {
  if (mode_blob_id_ != 0 && fd_ >= 0) {
    drmModeDestroyPropertyBlob(fd_, mode_blob_id_);
  }
  mode_blob_id_ = 0;
}

// ── attach / set_mode ────────────────────────────────────────────────────

drm::expected<void, std::error_code> Modeset::attach(AtomicRequest& req) const {
  if (auto r = req.add_property(crtc_id_, prop_mode_id_, mode_blob_id_); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = req.add_property(crtc_id_, prop_active_, 1); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = req.add_property(connector_id_, prop_connector_crtc_, crtc_id_); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  return {};
}

drm::expected<void, std::error_code> Modeset::set_mode(const drmModeModeInfo& mode) {
  drmModeModeInfo mode_copy = mode;
  uint32_t new_blob = 0;
  if (drmModeCreatePropertyBlob(fd_, &mode_copy, sizeof(mode_copy), &new_blob) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  if (mode_blob_id_ != 0) {
    drmModeDestroyPropertyBlob(fd_, mode_blob_id_);
  }
  mode_blob_id_ = new_blob;
  return {};
}

}  // namespace drm::modeset
