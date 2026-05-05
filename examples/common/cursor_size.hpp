// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cursor_size.hpp — per-output HiDPI cursor sizing for the examples.
//
// `DRM_CAP_CURSOR_WIDTH` is the driver's *maximum* supported cursor
// dimension, not a target — amdgpu reports 256 while Intel/i915 reports
// 64. Feeding that value straight into `Cursor::load` and
// `RendererConfig::preferred_size` gives a sprite that's correct on
// Intel and conspicuously oversized on amdgpu.
//
// Sprite size and buffer size are independent:
//   * sprite — what libxcursor picks off disk for `Cursor::load`. Free
//     to be any value, including below 64; smaller sprites get centered
//     inside the buffer by `cursor::Renderer::blit_frame`.
//   * buffer — what the kernel cursor plane actually scans out. Must
//     be one of the sizes the driver accepts: i915 hard-codes 64,
//     amdgpu DC takes only 64 / 128 / 256, anything else EINVALs the
//     per-frame atomic commit. So buffer is always ≥ 64.
//
// `cursor_sizing_for_output` derives DPI from the connector's physical
// size (mmWidth from EDID) and the active mode resolution, snaps to
// the conventional 96 / 192 / 288 ladder via round(dpi / 96), and
// multiplies a caller-chosen base logical size by that scale to get
// the sprite size. The buffer is then snapped up to the next power of
// two ≥ 64 to satisfy the cursor plane, and clamped to the driver cap.
// The XCURSOR_SIZE convention is 24 logical px; GNOME/KDE default to
// 32. Pass a smaller `base_logical` (e.g. 16) for a visibly smaller
// cursor that still scales with DPI.
//
// When the connector reports a zero physical size (some virtualized /
// VNC drivers) the helper falls back to scale = 1 — sprite stays at
// `base_logical`, buffer still snaps up to the hardware floor.

#pragma once

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cmath>
#include <cstdint>

namespace drm::examples {

struct CursorSizing {
  std::uint32_t sprite{0};  // → drm::cursor::Cursor::load target_size
  std::uint32_t buffer{0};  // → drm::cursor::RendererConfig::preferred_size
};

[[nodiscard]] inline CursorSizing cursor_sizing_for_output(int fd, std::uint32_t connector_id,
                                                           const drmModeModeInfo& mode,
                                                           std::uint32_t base_logical,
                                                           std::uint64_t cap_w) {
  std::uint32_t mm_w = 0;
  if (auto* c = drmModeGetConnector(fd, connector_id)) {
    mm_w = c->mmWidth;
    drmModeFreeConnector(c);
  }

  std::uint32_t scale = 1;
  if (mm_w != 0 && mode.hdisplay != 0) {
    const double dpi = static_cast<double>(mode.hdisplay) * 25.4 / static_cast<double>(mm_w);
    const long stepped = std::lround(dpi / 96.0);
    if (stepped > 1) {
      scale = static_cast<std::uint32_t>(stepped);
    }
  }

  const std::uint32_t sprite = base_logical * scale;

  // Buffer floor: i915 only accepts 64, amdgpu DC only 64 / 128 / 256.
  // Snap up to the next power of two ≥ 64 so the per-frame atomic
  // commit doesn't EINVAL, then clamp oversize HiDPI requests to the
  // driver cap.
  std::uint32_t buffer = 64U;
  while (buffer < sprite) {
    buffer *= 2U;
  }
  if (cap_w != 0 && buffer > cap_w) {
    buffer = static_cast<std::uint32_t>(cap_w);
  }

  return {sprite, buffer};
}

}  // namespace drm::examples
