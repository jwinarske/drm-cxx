// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

#include <xf86drmMode.h>

namespace drm {

struct ModeInfo {
  drmModeModeInfo drm_mode;
  uint32_t width() const noexcept;
  uint32_t height() const noexcept;
  uint32_t refresh() const noexcept;
  bool preferred() const noexcept;
};

[[nodiscard]] std::expected<ModeInfo, std::error_code>
  select_preferred_mode(std::span<const drmModeModeInfo> modes);

} // namespace drm
