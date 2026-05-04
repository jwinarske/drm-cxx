// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "theme.hpp"

#include <drm-cxx/detail/expected.hpp>

// toml++ pulls a large transitive header pile; isolated to this TU so
// public consumers of <drm-cxx/csd/theme.hpp> don't pay for it. The
// NOLINT mirrors capture/snapshot.cpp's <blend2d/blend2d.h> umbrella —
// misc-include-cleaner can't trace toml::table / toml::parse_error /
// toml::node_view back through toml++/impl/, so the use sites below
// are wrapped in a NOLINT block too.
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <toml++/toml.hpp>  // NOLINT(misc-include-cleaner)
#include <utility>

namespace drm::csd {

// NOLINTBEGIN(misc-include-cleaner) — toml::* arrives via the umbrella
// <toml++/toml.hpp> at the top of the file; the cleaner doesn't trace
// through toml++/impl/ so suppress its per-symbol complaints here.
namespace {

drm::unexpected<std::error_code> bad_arg() {
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
}

std::optional<std::uint8_t> parse_hex_byte(char hi, char lo) {
  auto digit = [](char c) -> std::optional<int> {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
      return 10 + (c - 'A');
    }
    return std::nullopt;
  };
  auto h = digit(hi);
  auto l = digit(lo);
  if (!h || !l) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>((*h << 4) | *l);
}

// Each helper updates `out` only on a successful parse; missing keys
// leave the base value intact, which is how load_theme_*'s "patch on
// top of base" semantics fall out.

bool extract_int(const toml::node_view<const toml::node>& node, int& out) {
  if (auto v = node.value<std::int64_t>()) {
    out = static_cast<int>(*v);
    return true;
  }
  return false;
}

bool extract_double(const toml::node_view<const toml::node>& node, double& out) {
  if (auto v = node.value<double>()) {
    out = *v;
    return true;
  }
  if (auto v = node.value<std::int64_t>()) {
    out = static_cast<double>(*v);
    return true;
  }
  return false;
}

bool extract_string(const toml::node_view<const toml::node>& node, std::string& out) {
  if (auto v = node.value<std::string>()) {
    out = *v;
    return true;
  }
  return false;
}

drm::expected<void, std::error_code> extract_color(const toml::node_view<const toml::node>& node,
                                                   Color& out) {
  if (!node) {
    return {};  // missing key: keep base value
  }
  std::string hex;
  if (!extract_string(node, hex)) {
    return bad_arg();
  }
  auto parsed = Color::from_hex(hex);
  if (!parsed) {
    return drm::unexpected<std::error_code>(parsed.error());
  }
  out = *parsed;
  return {};
}

drm::expected<void, std::error_code> extract_button(const toml::node_view<const toml::node>& node,
                                                    Theme::Buttons::Button& out) {
  if (!node) {
    return {};
  }
  if (auto e = extract_color(node["fill"], out.fill); !e.has_value()) {
    return e;
  }
  if (auto e = extract_color(node["hover"], out.hover); !e.has_value()) {
    return e;
  }
  return {};
}

drm::expected<Theme, std::error_code> apply_table(const toml::table& tbl, Theme base) {
  auto root = toml::node_view<const toml::node>(tbl);

  extract_string(root["name"], base.name);
  extract_int(root["corner_radius"], base.corner_radius);
  extract_double(root["noise_amplitude"], base.noise_amplitude);
  extract_int(root["shadow_extent"], base.shadow_extent);
  extract_int(root["animation_duration_ms"], base.animation_duration_ms);

  if (auto title_bar = root["title_bar"]) {
    extract_int(title_bar["height"], base.title_bar.height);
    extract_string(title_bar["font"], base.title_bar.font);
    extract_int(title_bar["font_size"], base.title_bar.font_size);
  }

  if (auto colors = root["colors"]) {
    if (auto e = extract_color(colors["panel_top"], base.colors.panel_top); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
    if (auto e = extract_color(colors["panel_bottom"], base.colors.panel_bottom); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
    if (auto e = extract_color(colors["rim_focused"], base.colors.rim_focused); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
    if (auto e = extract_color(colors["rim_blurred"], base.colors.rim_blurred); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
    if (auto e = extract_color(colors["shadow"], base.colors.shadow); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
    if (auto e = extract_color(colors["title_text"], base.colors.title_text); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
    if (auto e = extract_color(colors["title_shadow"], base.colors.title_shadow); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
  }

  if (auto buttons = root["buttons"]) {
    if (auto e = extract_button(buttons["close"], base.buttons.close); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
    if (auto e = extract_button(buttons["minimize"], base.buttons.minimize); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
    if (auto e = extract_button(buttons["maximize"], base.buttons.maximize); !e.has_value()) {
      return drm::unexpected<std::error_code>(e.error());
    }
  }

  return base;
}

}  // namespace

drm::expected<Color, std::error_code> Color::from_hex(std::string_view hex) {
  if (hex.empty() || hex.front() != '#') {
    return bad_arg();
  }
  auto body = hex.substr(1);
  if (body.size() != 6 && body.size() != 8) {
    return bad_arg();
  }
  Color out;
  auto r = parse_hex_byte(body[0], body[1]);
  auto g = parse_hex_byte(body[2], body[3]);
  auto b = parse_hex_byte(body[4], body[5]);
  if (!r || !g || !b) {
    return bad_arg();
  }
  out.r = *r;
  out.g = *g;
  out.b = *b;
  if (body.size() == 8) {
    auto a = parse_hex_byte(body[6], body[7]);
    if (!a) {
      return bad_arg();
    }
    out.a = *a;
  } else {
    out.a = 0xFF;
  }
  return out;
}

drm::expected<Theme, std::error_code> load_theme_string(std::string_view toml, Theme base) {
  // toml++'s parse_result has try_get_table()-style accessors, but the
  // simplest path is to wrap the parse in a try/catch — a parse failure
  // throws toml::parse_error in the C++17 build mode it ships with.
  try {
    auto tbl = toml::parse(toml);
    return apply_table(tbl, std::move(base));
  } catch (const toml::parse_error&) {
    return bad_arg();
  }
}

drm::expected<Theme, std::error_code> load_theme_file(std::string_view path, Theme base) {
  std::ifstream in{std::string(path)};
  if (!in.is_open()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  std::stringstream buf;
  buf << in.rdbuf();
  return load_theme_string(buf.str(), std::move(base));
}

namespace {

Theme make_glass_default() {
  Theme t;
  t.name = "glass-default";
  t.corner_radius = 8;
  t.noise_amplitude = 0.04;
  t.shadow_extent = 24;
  t.animation_duration_ms = 180;

  t.title_bar.height = 28;
  t.title_bar.font = "Inter, sans-serif";
  t.title_bar.font_size = 13;

  t.colors.panel_top = {0xFF, 0xFF, 0xFF, 0x73};
  t.colors.panel_bottom = {0xFF, 0xFF, 0xFF, 0x26};
  t.colors.rim_focused = {0xFF, 0xFF, 0xFF, 0x99};
  t.colors.rim_blurred = {0xC8, 0xC8, 0xC8, 0x4C};
  t.colors.shadow = {0x00, 0x00, 0x00, 0x8C};
  t.colors.title_text = {0x1E, 0x1E, 0x1E, 0xFF};
  t.colors.title_shadow = {0xFF, 0xFF, 0xFF, 0x66};

  t.buttons.close.fill = {0xFF, 0x5F, 0x56, 0xFF};
  t.buttons.close.hover = {0xFF, 0x8A, 0x82, 0xFF};
  t.buttons.minimize.fill = {0xFF, 0xBD, 0x2E, 0xFF};
  t.buttons.minimize.hover = {0xFF, 0xD1, 0x66, 0xFF};
  t.buttons.maximize.fill = {0x28, 0xC9, 0x40, 0xFF};
  t.buttons.maximize.hover = {0x5E, 0xE0, 0x71, 0xFF};

  return t;
}

// Tier 1: cheaper shadow + shorter focus animation to keep the
// software-composited path responsive on i.MX8 / RK3399 / Mali-class
// silicon. Same color palette as glass-default — only the cost knobs
// move.
Theme make_glass_lite() {
  Theme t = make_glass_default();
  t.name = "glass-lite";
  t.noise_amplitude = 0.02;
  t.shadow_extent = 12;
  t.animation_duration_ms = 120;
  return t;
}

// Tier 2: shadow off entirely (the /dev/fb0 blit path can't afford
// per-frame blur). Animation collapses to an instant cross-fade;
// callers that disable animations altogether get a single redraw.
Theme make_glass_minimal() {
  Theme t = make_glass_default();
  t.name = "glass-minimal";
  t.noise_amplitude = 0.0;
  t.shadow_extent = 0;
  t.animation_duration_ms = 0;
  t.colors.shadow = {0, 0, 0, 0};
  return t;
}

}  // namespace

const Theme& glass_default_theme() {
  static const Theme t = make_glass_default();
  return t;
}

const Theme& glass_lite_theme() {
  static const Theme t = make_glass_lite();
  return t;
}

const Theme& glass_minimal_theme() {
  static const Theme t = make_glass_minimal();
  return t;
}

// NOLINTEND(misc-include-cleaner)

}  // namespace drm::csd