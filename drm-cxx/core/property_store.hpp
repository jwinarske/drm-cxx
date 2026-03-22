// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace drm {

class PropertyStore {
public:
  std::expected<void, std::error_code>
    cache_properties(int fd, uint32_t object_id, uint32_t object_type);

  [[nodiscard]] std::expected<uint32_t, std::error_code>
    property_id(uint32_t object_id, std::string_view name) const;

private:
  // object_id -> (property_name -> property_id)
  std::unordered_map<uint32_t,
    std::unordered_map<std::string, uint32_t>> store_;
};

} // namespace drm
