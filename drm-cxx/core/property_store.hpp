// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
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
};

class PropertyStore {
public:
  std::expected<void, std::error_code>
    cache_properties(int fd, uint32_t object_id, uint32_t object_type);

  [[nodiscard]] std::expected<uint32_t, std::error_code>
    property_id(uint32_t object_id, std::string_view name) const;

  [[nodiscard]] std::expected<uint64_t, std::error_code>
    property_value(uint32_t object_id, std::string_view name) const;

  [[nodiscard]] const std::vector<PropertyInfo>*
    properties(uint32_t object_id) const;

  void clear() noexcept;

private:
  // object_id -> list of properties
  std::unordered_map<uint32_t, std::vector<PropertyInfo>> store_;
};

} // namespace drm
