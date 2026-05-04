// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/window_state.hpp — per-window inputs to the CSD renderer.
//
// WindowState is the small POD the shell hands to Renderer::draw on
// every paint pass. It carries the bits the glass theme reads: title
// string for the title bar, focus flag for the rim color, hover for
// the button highlight, and a dirty bitfield reserved for future
// partial-redraw paths.
//
// V1 of the renderer always paints the full decoration regardless of
// the dirty bits — clearing the target buffer and re-running the glass
// pipeline is cheaper at typical decoration sizes (≤ 1024×64 title bar
// or ≤ 256² for buttons) than the bookkeeping a damage-tracked path
// would need. The bits ride on the type so a later partial-redraw pass
// can be added without breaking the shell's call sites.
//
// Header-only; pulled into Surface-using headers without dragging in
// Blend2D or any drm-cxx detail.

#pragma once

#include <cstdint>
#include <string>

namespace drm::csd {

// Which decoration button the cursor is currently over (if any). The
// renderer paints the matching button with its `hover` color from the
// theme; everything else uses the `fill` color.
enum class HoverButton : std::uint8_t {
  None,
  Close,
  Minimize,
  Maximize,
};

// Bitfield for renderer's partial-redraw tracking. V1 always paints
// kDirtyAll; the named bits are here so the shell can already write
// the right value when the partial-redraw pass lands.
enum DirtyBit : std::uint32_t {
  kDirtyTitle = 1U << 0U,
  kDirtyFocus = 1U << 1U,
  kDirtyHover = 1U << 2U,
  kDirtyGeometry = 1U << 3U,
  kDirtyAll = ~0U,
};

struct WindowState {
  std::string title;
  bool focused{false};
  HoverButton hover{HoverButton::None};
  std::uint32_t dirty{kDirtyAll};
};

}  // namespace drm::csd
