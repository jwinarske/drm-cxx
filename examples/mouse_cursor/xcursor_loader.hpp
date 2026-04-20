// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// XCursor theme loader for the mouse_cursor example.
// Loads pre-rasterized cursors from installed XCursor themes (e.g. Adwaita)
// via libxcursor. No X11 display connection is required.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

struct CursorFrame {
  std::vector<uint32_t> pixels;  // ARGB8888, tightly packed, width*height
  uint32_t width{};
  uint32_t height{};
  int xhot{};
  int yhot{};
  uint32_t delay_ms{};  // 0 = static; >0 = animation frame duration
};

class LoadedCursor {
 public:
  // theme may be nullptr — libxcursor then uses XCURSOR_THEME env or "default".
  // Returns nullopt if the cursor isn't found in any searched theme.
  static std::optional<LoadedCursor> load(const char* name, const char* theme, int size);

  [[nodiscard]] const CursorFrame& frame_at(uint64_t now_ms) const;

  [[nodiscard]] bool animated() const { return frames_.size() > 1; }
  [[nodiscard]] uint32_t cycle_ms() const { return cycle_ms_; }
  [[nodiscard]] std::size_t frame_count() const { return frames_.size(); }
  [[nodiscard]] const CursorFrame& first() const { return frames_.front(); }

 private:
  std::vector<CursorFrame> frames_;
  uint32_t cycle_ms_{0};
};
