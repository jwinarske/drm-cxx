// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cursor/cursor.hpp — rasterized cursor with (optional) animation.
//
// Cursor is the pure-CPU half of the module: no fds, no KMS, no
// libdrm. It holds the ARGB8888 pixels for every frame of a loaded
// cursor, the hotspot and timing metadata, and a deterministic
// frame picker keyed off elapsed time. drm::cursor::Renderer
// rasterizes these frames onto a DRM plane; tests poke at
// frame_at() directly without needing a display.
//
// Pixel ownership: all frames live in one contiguous vector so an
// animated cursor costs exactly one heap allocation, and Frame
// structs are cheap views (span + a few integers) into that
// storage. Callers read frames by const reference; do not cache
// the reference across a Cursor move.

#pragma once

#include "detail/expected.hpp"
#include "detail/span.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>

namespace drm::cursor {

class Theme;
struct ThemeResolution;

/// One frame of a (possibly animated) cursor. Pixels are ARGB8888,
/// tightly packed, row-major. `delay` is zero for static cursors.
struct Frame {
  drm::span<const std::uint32_t> pixels;
  std::uint32_t width;
  std::uint32_t height;
  int xhot;
  int yhot;
  std::chrono::milliseconds delay;
};

/// A loaded cursor. Immutable after load — every accessor is const.
class Cursor {
 public:
  /// Load from a resolved theme entry. Picks the nearest available
  /// size that is >= requested_size; if no larger size is available,
  /// picks the largest and leaves scaling to the caller (Renderer
  /// centers smaller cursors into the plane buffer).
  [[nodiscard]] static drm::expected<Cursor, std::error_code> load(const ThemeResolution& resolved,
                                                                   std::uint32_t requested_size);

  /// Convenience: theme.resolve() + Cursor::load() in one call.
  [[nodiscard]] static drm::expected<Cursor, std::error_code> load(const Theme& theme,
                                                                   std::string_view cursor_name,
                                                                   std::string_view preferred_theme,
                                                                   std::uint32_t requested_size);

  Cursor(Cursor&&) noexcept;
  Cursor& operator=(Cursor&&) noexcept;
  Cursor(const Cursor&) = delete;
  Cursor& operator=(const Cursor&) = delete;
  ~Cursor();

  [[nodiscard]] const Frame& first() const noexcept;
  [[nodiscard]] const Frame& at(std::size_t index) const noexcept;
  [[nodiscard]] std::size_t frame_count() const noexcept;
  [[nodiscard]] bool animated() const noexcept;
  [[nodiscard]] std::chrono::milliseconds cycle() const noexcept;

  /// Deterministic frame picker. `elapsed` is time since the Cursor
  /// started displaying (Renderer captures its own start point and
  /// passes elapsed since then). For a static cursor or a cursor
  /// with a zero-length cycle, it always returns first().
  [[nodiscard]] const Frame& frame_at(std::chrono::milliseconds elapsed) const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit Cursor(std::unique_ptr<Impl> impl);
};

}  // namespace drm::cursor