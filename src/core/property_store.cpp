// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "property_store.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <vector>

namespace drm {

drm::expected<void, std::error_code> PropertyStore::cache_properties(const int fd,
                                                                     const uint32_t object_id,
                                                                     const uint32_t object_type) {
  auto* props = drmModeObjectGetProperties(fd, object_id, object_type);
  if (props == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  auto& entries = store_.try_emplace(object_id).first->second;
  entries.clear();
  entries.reserve(props->count_props);

  const auto prop_ids = drm::span<const uint32_t>(props->props, props->count_props);
  const auto prop_vals = drm::span<const uint64_t>(props->prop_values, props->count_props);

  for (uint32_t i = 0; i < props->count_props; ++i) {
    auto* prop = drmModeGetProperty(fd, prop_ids[i]);
    if (prop == nullptr) {
      continue;
    }

    entries.push_back(PropertyInfo{
        prop_ids[i],
        prop->name,
        prop_vals[i],
        prop->flags,
    });

    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
  return {};
}

drm::expected<uint32_t, std::error_code> PropertyStore::property_id(
    const uint32_t object_id, const std::string_view name) const {
  const auto it = store_.find(object_id);
  if (it == store_.end()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  for (const auto& prop : it->second) {
    if (prop.name == name) {
      return prop.id;
    }
  }

  return drm::unexpected<std::error_code>(
      std::make_error_code(std::errc::no_such_file_or_directory));
}

drm::expected<uint64_t, std::error_code> PropertyStore::property_value(
    const uint32_t object_id, const std::string_view name) const {
  const auto it = store_.find(object_id);
  if (it == store_.end()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  for (const auto& prop : it->second) {
    if (prop.name == name) {
      return prop.value;
    }
  }

  return drm::unexpected<std::error_code>(
      std::make_error_code(std::errc::no_such_file_or_directory));
}

drm::expected<bool, std::error_code> PropertyStore::is_immutable(
    const uint32_t object_id, const std::string_view name) const {
  const auto it = store_.find(object_id);
  if (it == store_.end()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  for (const auto& prop : it->second) {
    if (prop.name == name) {
      return (prop.flags & DRM_MODE_PROP_IMMUTABLE) != 0U;
    }
  }

  return drm::unexpected<std::error_code>(
      std::make_error_code(std::errc::no_such_file_or_directory));
}

const std::vector<PropertyInfo>* PropertyStore::properties(const uint32_t object_id) const {
  const auto it = store_.find(object_id);
  if (it == store_.end()) {
    return nullptr;
  }
  return &it->second;
}

void PropertyStore::clear() noexcept {
  store_.clear();
}

}  // namespace drm
