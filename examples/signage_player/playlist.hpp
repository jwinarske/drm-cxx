// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// playlist.hpp — toml-backed playlist for signage_player.
//
// One Playlist owns a cyclic list of SlideDesc entries plus an optional
// OverlayDesc. Slides drive the background layer (GbmBufferSource);
// the overlay drives a DumbBufferSource-backed text layer. Parse via
// Playlist::load(toml_path) → drm::expected<Playlist, error_code>.
//
// Schema (TOML):
//
//     [[slide]]
//     kind = "color"          # one of: "color", "png", "blend2d", "thorvg"
//     color = "#ff8800"       # required when kind="color"
//     source = "path.png"     # required when kind="png" | "blend2d" | "thorvg"
//     duration_ms = 2000      # optional, default 2000
//
//     [overlay]               # optional
//     kind = "text"           # currently the only supported overlay kind
//     text = "drm-cxx signage demo"
//     font_size = 32          # optional, default 32
//     fg_color = "#ffffff"    # optional, default white
//     bg_color = "#00000080"  # optional, default transparent
//
// At least one slide is required. The parser rejects unknown `kind`
// values and malformed color literals.

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace signage {

/// Slide-content backend. Only `Color` is actually rendered by the
/// scaffold; the PNG / Blend2D / ThorVG variants are parsed and
/// stored so the later content-renderer commit can fill them in
/// without re-visiting the parser.
enum class SlideKind : std::uint8_t { Color, Png, Blend2D, Thorvg };

/// Packed 0xAARRGGBB. The `alpha` channel defaults to opaque (0xFF)
/// when the TOML literal is `#RRGGBB`, otherwise parsed from
/// `#RRGGBBAA`.
using Argb = std::uint32_t;

struct SlideDesc {
  SlideKind kind{SlideKind::Color};
  /// For `kind == Color`: the fill colour. Unused otherwise.
  Argb color{0xFF000000};
  /// For `kind == Png | Blend2D | Thorvg`: filesystem path or demo
  /// identifier. Unused for `Color`.
  std::string source;
  std::uint32_t duration_ms{2000};
};

struct OverlayDesc {
  std::string text;
  std::uint32_t font_size{32};
  Argb fg_color{0xFFFFFFFFU};
  Argb bg_color{0x00000000U};
};

class Playlist {
 public:
  [[nodiscard]] static drm::expected<Playlist, std::error_code> load(const std::string& toml_path);

  /// Parse TOML source text directly. Test hook; `load` delegates here
  /// after reading the file.
  [[nodiscard]] static drm::expected<Playlist, std::error_code> parse(std::string_view toml_src);

  [[nodiscard]] const std::vector<SlideDesc>& slides() const noexcept { return slides_; }
  [[nodiscard]] const std::optional<OverlayDesc>& overlay() const noexcept { return overlay_; }

 private:
  std::vector<SlideDesc> slides_;
  std::optional<OverlayDesc> overlay_;
};

}  // namespace signage
