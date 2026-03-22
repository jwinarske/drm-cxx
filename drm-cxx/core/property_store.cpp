// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "property_store.hpp"

namespace drm {

std::expected<void, std::error_code>
PropertyStore::cache_properties([[maybe_unused]] int fd,
                                [[maybe_unused]] uint32_t object_id,
                                [[maybe_unused]] uint32_t object_type) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

std::expected<uint32_t, std::error_code>
PropertyStore::property_id([[maybe_unused]] uint32_t object_id,
                           [[maybe_unused]] std::string_view name) const {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace drm
