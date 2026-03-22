// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "property_store.hpp"

#include <cerrno>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace drm {

std::expected<void, std::error_code>
PropertyStore::cache_properties(int fd, uint32_t object_id, uint32_t object_type) {
  auto* props = drmModeObjectGetProperties(fd, object_id, object_type);
  if (!props) {
    return std::unexpected(std::error_code(errno, std::system_category()));
  }

  auto& entries = store_[object_id];
  entries.clear();
  entries.reserve(props->count_props);

  for (uint32_t i = 0; i < props->count_props; ++i) {
    auto* prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop) {
      continue;
    }

    entries.push_back(PropertyInfo{
      .id = props->props[i],
      .name = prop->name,
      .value = props->prop_values[i],
    });

    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
  return {};
}

std::expected<uint32_t, std::error_code>
PropertyStore::property_id(uint32_t object_id, std::string_view name) const {
  auto it = store_.find(object_id);
  if (it == store_.end()) {
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
  }

  for (const auto& prop : it->second) {
    if (prop.name == name) {
      return prop.id;
    }
  }

  return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
}

std::expected<uint64_t, std::error_code>
PropertyStore::property_value(uint32_t object_id, std::string_view name) const {
  auto it = store_.find(object_id);
  if (it == store_.end()) {
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
  }

  for (const auto& prop : it->second) {
    if (prop.name == name) {
      return prop.value;
    }
  }

  return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
}

const std::vector<PropertyInfo>*
PropertyStore::properties(uint32_t object_id) const {
  auto it = store_.find(object_id);
  if (it == store_.end()) {
    return nullptr;
  }
  return &it->second;
}

void PropertyStore::clear() noexcept {
  store_.clear();
}

} // namespace drm
