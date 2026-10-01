// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Private (not installed): one AddFB2 entry point that picks between the
// modifier'd and the legacy ioctl path, so every import site agrees on it.

#pragma once

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>

namespace drm::detail {

// True when the per-plane modifiers must ride DRM_MODE_FB_MODIFIERS.
//  - INVALID never does: it means "no explicit modifier", and forwarding it
//    through the flag is ill-formed.
//  - LINEAR does only when the driver took DRM_CAP_ADDFB2_MODIFIERS. Drivers
//    without it (i.MX LCDIF, tilcdc, starfive, ...) reject the flag outright
//    (EINVAL / ENOSYS), yet LINEAR is exactly their implicit layout, so the
//    legacy path imports the same buffer.
//  - Anything else (tiled / compressed) always does; a driver without the cap
//    cannot scan it out anyway, and the kernel says so.
[[nodiscard]] inline bool addfb2_needs_modifiers(int fd, std::uint64_t modifier) noexcept {
  if (modifier == DRM_FORMAT_MOD_INVALID) {
    return false;
  }
  if (modifier != DRM_FORMAT_MOD_LINEAR) {
    return true;
  }
  std::uint64_t cap = 0;
  return drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap != 0;
}

// drmModeAddFB2WithModifiers with the flag chosen by addfb2_needs_modifiers()
// on plane 0's modifier (all planes of one FB share a modifier). Returns the
// libdrm rc; errno is left as the ioctl set it.
[[nodiscard]] inline int add_fb2(int fd, std::uint32_t width, std::uint32_t height,
                                 std::uint32_t fourcc, const std::uint32_t handles[4],
                                 const std::uint32_t pitches[4], const std::uint32_t offsets[4],
                                 const std::uint64_t modifiers[4], std::uint32_t* fb_id,
                                 std::uint32_t flags = 0) noexcept {
  const bool use_modifiers = modifiers != nullptr && addfb2_needs_modifiers(fd, modifiers[0]);
  return drmModeAddFB2WithModifiers(
      fd, width, height, fourcc, handles, pitches, offsets, use_modifiers ? modifiers : nullptr,
      fb_id, use_modifiers ? (flags | DRM_MODE_FB_MODIFIERS) : (flags & ~DRM_MODE_FB_MODIFIERS));
}

}  // namespace drm::detail
