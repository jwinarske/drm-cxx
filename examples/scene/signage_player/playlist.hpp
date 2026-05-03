// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// playlist.hpp — toml-backed playlist for signage_player.
//
// One Playlist owns a cyclic list of SlideDesc entries plus optional
// OverlayDesc, TickerDesc, ClockDesc, and LogoDesc blocks. Slides drive
// the background layer (GbmBufferSource); the overlay drives a static
// DumbBufferSource-backed text layer; the ticker drives a third
// DumbBufferSource that repaints every frame with a scrolling marquee;
// the clock drives a fourth DumbBufferSource that repaints only when
// the formatted time string changes (once per minute with the default
// "%H:%M"); the logo drives a fifth DumbBufferSource painted once from
// a PNG. Parse via Playlist::load(toml_path).
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
//     [ticker]                # optional
//     text = "BREAKING NEWS  ·  more headlines  ·  "
//     font_size = 24          # optional, default 24
//     fg_color = "#ffffff"    # optional, default white
//     bg_color = "#000000c0"  # optional, default 75% black
//     pixels_per_second = 120 # optional, default 120
//
//     [clock]                 # optional
//     format = "%H:%M"        # optional strftime, default "%H:%M"
//     font_size = 48          # optional, default 48
//     fg_color = "#ffffff"    # optional, default white
//     bg_color = "#80000000"  # optional, default 50% black
//
//     [logo]                  # optional, top-left brand bug
//     path = "logo.png"       # required PNG path
//     width = 96              # optional, default 96
//     height = 96             # optional, default 96
//     fallback_color = "#ffffff80"  # optional; painted when Blend2D
//                             # is missing or PNG load fails
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
  /// For `kind == Color`: the fill color. Unused otherwise.
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

struct TickerDesc {
  std::string text;
  std::uint32_t font_size{24};
  Argb fg_color{0xFFFFFFFFU};
  /// Default 75% opaque black, so the marquee reads against most slides.
  Argb bg_color{0xC0000000U};
  /// Horizontal scroll velocity. The renderer multiplies elapsed time
  /// by this and modulos against a single text-pass width to produce a
  /// seamless loop.
  std::uint32_t pixels_per_second{120};
};

struct ClockDesc {
  /// strftime-style format. Default "%H:%M" gives the once-per-minute
  /// repaint cadence the example was designed to demonstrate; passing
  /// "%H:%M:%S" or similar will simply make repaints fire every second.
  std::string format{"%H:%M"};
  std::uint32_t font_size{48};
  Argb fg_color{0xFFFFFFFFU};
  /// Default 50% opaque black, slightly lighter than the ticker's 75%
  /// because the clock pad sits over slide content rather than its own
  /// bottom band.
  Argb bg_color{0x80000000U};
};

struct LogoDesc {
  /// PNG file path. Loaded once at scene construction (and again on
  /// session resume). Required.
  std::string path;
  /// Layer rectangle. The image is scaled-to-fit while preserving
  /// the aspect ratio; letterboxing pixels are left at the fallback fill.
  std::uint32_t width{96};
  std::uint32_t height{96};
  /// Painted under the PNG so the layer is still visible when Blend2D
  /// isn't compiled in or the PNG fails to load. Default is fully
  /// transparent, so a successful PNG load looks identical with or
  /// without the fallback in place.
  Argb fallback_color{0x00000000U};
};

class Playlist {
 public:
  [[nodiscard]] static drm::expected<Playlist, std::error_code> load(const std::string& toml_path);

  /// Parse TOML source text directly. Test hook; `load` delegates here
  /// after reading the file.
  [[nodiscard]] static drm::expected<Playlist, std::error_code> parse(std::string_view toml_src);

  [[nodiscard]] const std::vector<SlideDesc>& slides() const noexcept { return slides_; }
  [[nodiscard]] const std::optional<OverlayDesc>& overlay() const noexcept { return overlay_; }
  [[nodiscard]] const std::optional<TickerDesc>& ticker() const noexcept { return ticker_; }
  [[nodiscard]] const std::optional<ClockDesc>& clock() const noexcept { return clock_; }
  [[nodiscard]] const std::optional<LogoDesc>& logo() const noexcept { return logo_; }

 private:
  std::vector<SlideDesc> slides_;
  std::optional<OverlayDesc> overlay_;
  std::optional<TickerDesc> ticker_;
  std::optional<ClockDesc> clock_;
  std::optional<LogoDesc> logo_;
};

}  // namespace signage
