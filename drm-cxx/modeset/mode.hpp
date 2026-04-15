// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <xf86drmMode.h>

#include <cstdint>
#include <system_error>
#include <vector>

namespace drm {

struct ModeInfo {
  drmModeModeInfo drm_mode{};

  [[nodiscard]] uint32_t width() const noexcept;
  [[nodiscard]] uint32_t height() const noexcept;
  [[nodiscard]] uint32_t refresh() const noexcept;
  [[nodiscard]] bool preferred() const noexcept;
  [[nodiscard]] bool interlaced() const noexcept;
  [[nodiscard]] uint32_t clock_khz() const noexcept;
};

// Select the preferred mode from a list.
[[nodiscard]] drm::expected<ModeInfo, std::error_code> select_preferred_mode(
    drm::span<const drmModeModeInfo> modes);

// Select the best mode matching a target resolution.
// If exact match not found, returns the closest resolution at highest refresh.
[[nodiscard]] drm::expected<ModeInfo, std::error_code> select_mode(
    drm::span<const drmModeModeInfo> modes, uint32_t target_width, uint32_t target_height,
    uint32_t target_refresh = 0);

// Get all modes as ModeInfo structs.
[[nodiscard]] std::vector<ModeInfo> get_all_modes(drm::span<const drmModeModeInfo> modes);

}  // namespace drm
