// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "connector_info.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

namespace drm::display {

std::expected<ConnectorInfo, std::error_code> parse_edid(std::span<const uint8_t> edid_blob);

}  // namespace drm::display
