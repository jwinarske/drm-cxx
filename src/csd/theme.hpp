// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/theme.hpp — drm::csd theme aggregate + TOML loader.
//
// The Theme struct is the source of truth for every visual parameter
// the decoration renderer reads. Color tokens parse once at load time
// into drm::csd::Color (a POD r/g/b/a struct), so the renderer never
// reparses hex strings on the draw path.
//
// Three built-in variants ship: glass_default (Tier 0, plane-per-
// decoration desktop / well-provisioned ARM), glass_lite (Tier 1,
// 2-plane software-composited mid-range ARM), and glass_minimal
// (Tier 2, /dev/fb0 fallback). They are returned by reference from
// glass_*_theme() free functions and carry no global state.
//
// Theme is intentionally Blend2D-free: the BL* boundary lives in the
// renderer translation unit. Consumers that only care about loading or
// programmatically constructing themes don't pull in <blend2d.h>.

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace drm::csd {

// Premultiplication is the renderer's responsibility — Color stores
// straight, gamma-encoded sRGB components plus an opacity byte.
struct Color {
  std::uint8_t r{};
  std::uint8_t g{};
  std::uint8_t b{};
  std::uint8_t a{};

  // Parse "#RRGGBB" (alpha defaults to 0xFF) or "#RRGGBBAA". Rejects
  // everything else (missing '#', non-hex digits, wrong length) with
  // std::errc::invalid_argument.
  static drm::expected<Color, std::error_code> from_hex(std::string_view hex);

  // Pack into BLRgba32's native 0xAARRGGBB layout so the renderer can
  // reinterpret_cast at the Blend2D boundary without per-pixel work.
  [[nodiscard]] std::uint32_t packed_argb() const noexcept {
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
  }

  friend bool operator==(Color a, Color b) noexcept {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
  }
  friend bool operator!=(Color a, Color b) noexcept { return !(a == b); }
};

struct Theme {
  std::string name;
  int corner_radius{};

  struct TitleBar {
    int height{};
    std::string font;
    int font_size{};
  } title_bar;

  struct Colors {
    Color panel_top;
    Color panel_bottom;
    Color rim_focused;
    Color rim_blurred;
    Color shadow;
    Color title_text;
    Color title_shadow;
  } colors;

  struct Buttons {
    struct Button {
      Color fill;
      Color hover;
    };
    Button close;
    Button minimize;
    Button maximize;
  } buttons;

  double noise_amplitude{};
  int shadow_extent{};
  int animation_duration_ms{};
};

// Parse a TOML theme from a file. Fields absent in the TOML keep their
// values from `base`, which lets a small user override file ride on
// top of a built-in (e.g. start from glass_default_theme(), patch the
// rim color). Malformed TOML, unknown color hex, or wrong-typed values
// return std::errc::invalid_argument; ENOENT surfaces from open as the
// system errc.
drm::expected<Theme, std::error_code> load_theme_file(std::string_view path, Theme base = {});

// Same semantics as load_theme_file but reads from an in-memory buffer
// (Wayland config-fd, embedded-resource, test fixture).
drm::expected<Theme, std::error_code> load_theme_string(std::string_view toml, Theme base = {});

// Built-in variants. Each is a function-local static initialized on
// first call; callers may bind by const reference and rely on the
// values living for the lifetime of the program.
const Theme& glass_default_theme();
const Theme& glass_lite_theme();
const Theme& glass_minimal_theme();

}  // namespace drm::csd