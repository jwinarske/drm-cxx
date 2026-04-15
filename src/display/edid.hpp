// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "connector_info.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cstdint>
#include <system_error>

namespace drm::display {

drm::expected<ConnectorInfo, std::error_code> parse_edid(drm::span<const uint8_t> edid_blob);

}  // namespace drm::display
