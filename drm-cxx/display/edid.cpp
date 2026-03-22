// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "edid.hpp"

namespace drm::display {

std::expected<ConnectorInfo, std::error_code>
parse_edid([[maybe_unused]] std::span<const uint8_t> edid_blob) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace drm::display
