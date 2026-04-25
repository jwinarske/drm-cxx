// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "playlist.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <toml++/toml.hpp>
#include <vector>

namespace signage {

namespace {

// Parse a hex-colour literal "#rrggbb" or "#rrggbbaa" into packed
// 0xAARRGGBB. Returns empty on malformed input.
std::optional<Argb> parse_color(std::string_view lit) noexcept {
  if (lit.empty() || lit.front() != '#') {
    return std::nullopt;
  }
  lit.remove_prefix(1);
  if (lit.size() != 6 && lit.size() != 8) {
    return std::nullopt;
  }
  std::uint32_t acc = 0;
  for (const char c : lit) {
    acc <<= 4U;
    if (c >= '0' && c <= '9') {
      acc |= static_cast<std::uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      acc |= static_cast<std::uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      acc |= static_cast<std::uint32_t>(c - 'A' + 10);
    } else {
      return std::nullopt;
    }
  }
  // Input is RRGGBB(AA); pack to AARRGGBB.
  if (lit.size() == 6) {
    return 0xFF000000U | acc;
  }
  const std::uint32_t rgb = acc >> 8U;
  const std::uint32_t a = acc & 0xFFU;
  return (a << 24U) | rgb;
}

std::optional<SlideKind> parse_kind(std::string_view s) noexcept {
  if (s == "color") {
    return SlideKind::Color;
  }
  if (s == "png") {
    return SlideKind::Png;
  }
  if (s == "blend2d") {
    return SlideKind::Blend2D;
  }
  if (s == "thorvg") {
    return SlideKind::Thorvg;
  }
  return std::nullopt;
}

drm::expected<SlideDesc, std::error_code> parse_slide(const toml::table& tbl) {
  SlideDesc s;

  const auto kind_lit = tbl["kind"].value<std::string>();
  if (!kind_lit) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  auto kind = parse_kind(*kind_lit);
  if (!kind) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  s.kind = *kind;

  if (s.kind == SlideKind::Color) {
    const auto color_lit = tbl["color"].value<std::string>();
    if (!color_lit) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    auto color = parse_color(*color_lit);
    if (!color) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    s.color = *color;
  } else {
    const auto source_lit = tbl["source"].value<std::string>();
    if (!source_lit || source_lit->empty()) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    s.source = *source_lit;
  }

  if (auto d = tbl["duration_ms"].value<std::int64_t>(); d && *d > 0) {
    s.duration_ms = static_cast<std::uint32_t>(*d);
  }
  return s;
}

drm::expected<OverlayDesc, std::error_code> parse_overlay(const toml::table& tbl) {
  OverlayDesc o;
  const auto kind_lit = tbl["kind"].value<std::string>();
  if (!kind_lit || *kind_lit != "text") {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  const auto text_lit = tbl["text"].value<std::string>();
  if (!text_lit) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  o.text = *text_lit;
  if (auto fs = tbl["font_size"].value<std::int64_t>(); fs && *fs > 0) {
    o.font_size = static_cast<std::uint32_t>(*fs);
  }
  if (auto fg = tbl["fg_color"].value<std::string>()) {
    auto parsed = parse_color(*fg);
    if (!parsed) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    o.fg_color = *parsed;
  }
  if (auto bg = tbl["bg_color"].value<std::string>()) {
    auto parsed = parse_color(*bg);
    if (!parsed) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    o.bg_color = *parsed;
  }
  return o;
}

}  // namespace

drm::expected<Playlist, std::error_code> Playlist::parse(std::string_view toml_src) {
  Playlist p;
  toml::table root;
  try {
    root = toml::parse(toml_src);
  } catch (const toml::parse_error&) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  const auto* slides = root["slide"].as_array();
  if (slides == nullptr || slides->empty()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  for (const auto& node : *slides) {
    const auto* tbl = node.as_table();
    if (tbl == nullptr) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    auto s = parse_slide(*tbl);
    if (!s) {
      return drm::unexpected<std::error_code>(s.error());
    }
    p.slides_.push_back(std::move(*s));
  }

  if (const auto* overlay_tbl = root["overlay"].as_table(); overlay_tbl != nullptr) {
    auto o = parse_overlay(*overlay_tbl);
    if (!o) {
      return drm::unexpected<std::error_code>(o.error());
    }
    p.overlay_ = std::move(*o);
  }

  return p;
}

drm::expected<Playlist, std::error_code> Playlist::load(const std::string& toml_path) {
  std::ifstream f(toml_path);
  if (!f) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  std::ostringstream buf;
  buf << f.rdbuf();
  return parse(buf.str());
}

}  // namespace signage
