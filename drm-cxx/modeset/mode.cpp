// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "mode.hpp"

namespace drm {

uint32_t ModeInfo::width() const noexcept { return drm_mode.hdisplay; }
uint32_t ModeInfo::height() const noexcept { return drm_mode.vdisplay; }
uint32_t ModeInfo::refresh() const noexcept { return drm_mode.vrefresh; }
bool ModeInfo::preferred() const noexcept {
  return drm_mode.type & DRM_MODE_TYPE_PREFERRED;
}

std::expected<ModeInfo, std::error_code>
select_preferred_mode([[maybe_unused]] std::span<const drmModeModeInfo> modes) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace drm
