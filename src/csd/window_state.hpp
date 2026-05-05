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

// Bitfield masks for WindowState::dirty. V1 always paints k_dirty_all;
// the named bits are here so the shell can already write the right
// value when the partial-redraw pass lands. Plain constexpr (not enum
// class) because the field is a uint32_t and the typical use is
// `state.dirty |= k_dirty_focus` — no scoping value over the noise of
// per-call casts to / from the underlying type.
inline constexpr std::uint32_t k_dirty_title = 1U << 0U;
inline constexpr std::uint32_t k_dirty_focus = 1U << 1U;
inline constexpr std::uint32_t k_dirty_hover = 1U << 2U;
inline constexpr std::uint32_t k_dirty_geometry = 1U << 3U;
inline constexpr std::uint32_t k_dirty_animation = 1U << 4U;
inline constexpr std::uint32_t k_dirty_all = ~0U;

// Sentinel that tells the renderer to derive the continuous progress
// from the binary `focused` / `hover` fields. Lets v1 callers (no
// animator) keep their existing field setup and still see correct
// visuals; the animator path overwrites the sentinel with a value in
// [0, 1] via WindowAnim::apply_to.
inline constexpr float k_progress_unset = -1.0F;

struct WindowState {
  std::string title;
  bool focused{false};
  HoverButton hover{HoverButton::None};
  std::uint32_t dirty{k_dirty_all};

  // Continuous animator outputs read by the renderer.
  // `focus_progress` is the eased weight in [0, 1] from blurred (0) to
  // focused (1). `hover_progress` is the eased weight in [0, 1] from
  // fill (0) to hover (1) for whichever button is named in `hover`.
  // Both default to k_progress_unset; the renderer falls back to the
  // binary fields in that case so unanimated callers stay correct.
  float focus_progress{k_progress_unset};
  float hover_progress{k_progress_unset};
};

}  // namespace drm::csd
