// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "mode.hpp"

#include <drm_mode.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <system_error>
#include <vector>

namespace drm {

uint32_t ModeInfo::width() const noexcept {
  return drm_mode.hdisplay;
}
uint32_t ModeInfo::height() const noexcept {
  return drm_mode.vdisplay;
}
uint32_t ModeInfo::refresh() const noexcept {
  return drm_mode.vrefresh;
}

bool ModeInfo::preferred() const noexcept {
  return (drm_mode.type & DRM_MODE_TYPE_PREFERRED) != 0;
}

bool ModeInfo::interlaced() const noexcept {
  return (drm_mode.flags & DRM_MODE_FLAG_INTERLACE) != 0;
}

uint32_t ModeInfo::clock_khz() const noexcept {
  return drm_mode.clock;
}

std::expected<ModeInfo, std::error_code> select_preferred_mode(
    std::span<const drmModeModeInfo> modes) {
  if (modes.empty()) {
    return std::unexpected(std::make_error_code(std::errc::no_such_device));
  }

  // First pass: look for a mode flagged as preferred
  for (const auto& m : modes) {
    if ((m.type & DRM_MODE_TYPE_PREFERRED) != 0U) {
      return ModeInfo{.drm_mode = m};
    }
  }

  // Fallback: pick highest resolution, then highest refresh
  const drmModeModeInfo* best = modes.data();
  for (const auto& m : modes) {
    uint64_t const area = static_cast<uint64_t>(m.hdisplay) * m.vdisplay;
    uint64_t const best_area = static_cast<uint64_t>(best->hdisplay) * best->vdisplay;
    if (area > best_area || (area == best_area && m.vrefresh > best->vrefresh)) {
      best = &m;
    }
  }

  return ModeInfo{.drm_mode = *best};
}

std::expected<ModeInfo, std::error_code> select_mode(std::span<const drmModeModeInfo> modes,
                                                     uint32_t target_width, uint32_t target_height,
                                                     uint32_t target_refresh) {
  if (modes.empty()) {
    return std::unexpected(std::make_error_code(std::errc::no_such_device));
  }

  const drmModeModeInfo* best = nullptr;
  uint64_t best_score = std::numeric_limits<uint64_t>::max();

  for (const auto& m : modes) {
    // Skip interlaced modes unless specifically targeting them
    if ((m.flags & DRM_MODE_FLAG_INTERLACE) != 0U) {
      continue;
    }

    auto dw = static_cast<uint64_t>((m.hdisplay > target_width) ? m.hdisplay - target_width
                                                                : target_width - m.hdisplay);
    auto dh = static_cast<uint64_t>((m.vdisplay > target_height) ? m.vdisplay - target_height
                                                                 : target_height - m.vdisplay);
    uint64_t score = (dw * dw) + (dh * dh);

    if (target_refresh > 0) {
      auto dr = static_cast<uint64_t>((m.vrefresh > target_refresh) ? m.vrefresh - target_refresh
                                                                    : target_refresh - m.vrefresh);
      score += dr * 100;  // Weight refresh match
    }

    if (score < best_score ||
        (score == best_score && (best != nullptr) && m.vrefresh > best->vrefresh)) {
      best = &m;
      best_score = score;
    }
  }

  if (best == nullptr) {
    return std::unexpected(std::make_error_code(std::errc::no_such_device));
  }

  return ModeInfo{.drm_mode = *best};
}

std::vector<ModeInfo> get_all_modes(std::span<const drmModeModeInfo> modes) {
  std::vector<ModeInfo> result;
  result.reserve(modes.size());
  for (const auto& m : modes) {
    result.push_back(ModeInfo{.drm_mode = m});
  }
  return result;
}

}  // namespace drm
