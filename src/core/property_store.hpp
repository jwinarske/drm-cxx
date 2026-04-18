// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace drm {

struct PropertyInfo {
  uint32_t id{};
  std::string name;
  uint64_t value{};
  // Raw DRM_MODE_PROP_* flags returned by drmModeGetProperty. Kept so
  // callers can distinguish immutable/range/enum/blob properties without
  // re-querying the kernel.
  uint32_t flags{};
};

class PropertyStore {
 public:
  drm::expected<void, std::error_code> cache_properties(int fd, uint32_t object_id,
                                                        uint32_t object_type);

  [[nodiscard]] drm::expected<uint32_t, std::error_code> property_id(uint32_t object_id,
                                                                     std::string_view name) const;

  [[nodiscard]] drm::expected<uint64_t, std::error_code> property_value(
      uint32_t object_id, std::string_view name) const;

  // True when the named property is advertised as DRM_MODE_PROP_IMMUTABLE.
  // Writing to such a property via an atomic commit is rejected by the
  // kernel with -EINVAL regardless of value, so callers building an
  // atomic request should skip it.
  [[nodiscard]] drm::expected<bool, std::error_code> is_immutable(uint32_t object_id,
                                                                  std::string_view name) const;

  [[nodiscard]] const std::vector<PropertyInfo>* properties(uint32_t object_id) const;

  void clear() noexcept;

 private:
  // object_id -> list of properties
  std::unordered_map<uint32_t, std::vector<PropertyInfo>> store_;
};

}  // namespace drm
