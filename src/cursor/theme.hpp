// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cursor/theme.hpp — XDG icon theme resolver for cursor loading.
//
// What Theme does:
//   - Walks the standard cursor search path (XCURSOR_PATH, then
//     $HOME/.icons, then /usr/share/icons) and indexes every
//     theme that advertises a cursors/ subdirectory.
//   - Parses each theme's index.theme and follows the Inherits=
//     key so that requesting a cursor from a theme that doesn't
//     ship that name falls back to a declared parent theme
//     (freedesktop.org icon-theme-spec §4).
//   - Resolves cursor aliases (e-resize ↔ right_side,
//     grabbing ↔ closedhand, and ~40 more defined by the
//     XCursor Resource Files convention) so callers can ask for
//     the modern name regardless of what the theme ships.
//
// What Theme does NOT do:
//   - Rasterize pixels. resolve() returns a file path and a theme
//     name; drm::cursor::Cursor::load() does the actual libxcursor
//     call. Keeps Theme cheap (no megabytes of pixel data) and
//     testable without a running display.

#pragma once

#include "detail/expected.hpp"
#include "detail/span.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace drm::cursor {

/// The outcome of resolving a cursor name against a Theme.
/// `theme_name` is what resolve() actually found the cursor in (which
/// may differ from the requested preferred_theme if we fell back
/// through the Inherits chain). `source` is the .cursor file on disk
/// that Cursor::load will open. `chain` records the full inheritance
/// walk that was considered, in order, for diagnostics.
struct ThemeResolution {
  std::string theme_name;
  std::filesystem::path source;
  std::vector<std::string> chain;
};

/// Resolver over the on-disk XCursor theme index. Cheap to construct
/// (directory walk + small metadata parse); no pixels loaded.
class Theme {
 public:
  /// Discovers themes using the standard search path. Fails only if
  /// no theme directories are readable at all — typical distros ship
  /// at least one, so this is almost always fine.
  [[nodiscard]] static drm::expected<Theme, std::error_code> discover();

  /// Test seam: override the search path explicitly. Used by the
  /// mock-theme integration tests in tests/cursor/.
  [[nodiscard]] static drm::expected<Theme, std::error_code> discover_with_paths(
      drm::span<const std::filesystem::path> search_paths);

  Theme(Theme&&) noexcept;
  Theme& operator=(Theme&&) noexcept;
  Theme(const Theme&) = delete;
  Theme& operator=(const Theme&) = delete;
  ~Theme();

  /// Resolve a cursor name to an on-disk file. `preferred_theme` may
  /// be empty, in which case the resolver uses $XCURSOR_THEME, then
  /// "default", then whatever comes first on the search path. Follows
  /// the Inherits= chain and the XCursor alias table.
  [[nodiscard]] drm::expected<ThemeResolution, std::error_code> resolve(
      std::string_view cursor_name, std::string_view preferred_theme) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit Theme(std::unique_ptr<Impl> impl);
};

}  // namespace drm::cursor