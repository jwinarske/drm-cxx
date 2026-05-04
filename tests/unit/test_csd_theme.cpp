// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include <drm-cxx/csd/theme.hpp>

#include <cstdint>
#include <gtest/gtest.h>
#include <string>

using drm::csd::Color;
using drm::csd::Theme;

// ── Color::from_hex ─────────────────────────────────────────────

TEST(CsdColorTest, ParsesSixDigitHex) {
  auto c = Color::from_hex("#FF8040");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->r, 0xFF);
  EXPECT_EQ(c->g, 0x80);
  EXPECT_EQ(c->b, 0x40);
  EXPECT_EQ(c->a, 0xFF);  // implicit opaque alpha
}

TEST(CsdColorTest, ParsesEightDigitHex) {
  auto c = Color::from_hex("#1E1E1E33");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->r, 0x1E);
  EXPECT_EQ(c->g, 0x1E);
  EXPECT_EQ(c->b, 0x1E);
  EXPECT_EQ(c->a, 0x33);
}

TEST(CsdColorTest, AcceptsLowercaseHexDigits) {
  auto c = Color::from_hex("#abcdefAB");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->r, 0xAB);
  EXPECT_EQ(c->g, 0xCD);
  EXPECT_EQ(c->b, 0xEF);
  EXPECT_EQ(c->a, 0xAB);
}

TEST(CsdColorTest, RejectsMalformedInput) {
  EXPECT_FALSE(Color::from_hex("").has_value());
  EXPECT_FALSE(Color::from_hex("FF8040").has_value());       // missing '#'
  EXPECT_FALSE(Color::from_hex("#FF80").has_value());        // too short
  EXPECT_FALSE(Color::from_hex("#FF8040FF00").has_value());  // too long
  EXPECT_FALSE(Color::from_hex("#GG8040").has_value());      // non-hex
  EXPECT_FALSE(Color::from_hex("#FF 8040").has_value());     // space
}

TEST(CsdColorTest, PackedArgbMatchesBLRgba32Layout) {
  Color c{0x12, 0x34, 0x56, 0x78};
  // BLRgba32 stores as 0xAARRGGBB on little-endian hosts.
  EXPECT_EQ(c.packed_argb(), static_cast<std::uint32_t>(0x78123456));
}

TEST(CsdColorTest, EqualityIsMemberwise) {
  Color a{1, 2, 3, 4};
  Color b{1, 2, 3, 4};
  Color c{1, 2, 3, 5};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

// ── load_theme_string ───────────────────────────────────────────

TEST(CsdLoadThemeStringTest, RoundTripsACompleteTheme) {
  auto toml = R"(
name            = "round-trip-glass"
corner_radius   = 12
noise_amplitude = 0.06
shadow_extent   = 32
animation_duration_ms = 220

[title_bar]
height    = 30
font      = "Cantarell, sans-serif"
font_size = 14

[colors]
panel_top    = "#FFFFFFAA"
panel_bottom = "#FFFFFF22"
rim_focused  = "#AAAAAA88"
rim_blurred  = "#BBBBBB44"
shadow       = "#0000007F"
title_text   = "#11111111"
title_shadow = "#FFFFFFAA"

[buttons.close]
fill  = "#FF0000FF"
hover = "#FF6666FF"

[buttons.minimize]
fill  = "#FFFF00FF"
hover = "#FFFF99FF"

[buttons.maximize]
fill  = "#00FF00FF"
hover = "#99FF99FF"
)";

  auto t = drm::csd::load_theme_string(toml);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->name, "round-trip-glass");
  EXPECT_EQ(t->corner_radius, 12);
  EXPECT_DOUBLE_EQ(t->noise_amplitude, 0.06);
  EXPECT_EQ(t->shadow_extent, 32);
  EXPECT_EQ(t->animation_duration_ms, 220);
  EXPECT_EQ(t->title_bar.height, 30);
  EXPECT_EQ(t->title_bar.font, "Cantarell, sans-serif");
  EXPECT_EQ(t->title_bar.font_size, 14);
  EXPECT_EQ(t->colors.panel_top, (Color{0xFF, 0xFF, 0xFF, 0xAA}));
  EXPECT_EQ(t->colors.shadow, (Color{0x00, 0x00, 0x00, 0x7F}));
  EXPECT_EQ(t->buttons.close.fill, (Color{0xFF, 0x00, 0x00, 0xFF}));
  EXPECT_EQ(t->buttons.maximize.hover, (Color{0x99, 0xFF, 0x99, 0xFF}));
}

TEST(CsdLoadThemeStringTest, MissingFieldsInheritFromBase) {
  auto base = drm::csd::glass_default_theme();
  auto overlay = R"(
corner_radius = 4
[colors]
rim_focused = "#FF000080"
)";
  auto t = drm::csd::load_theme_string(overlay, base);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->corner_radius, 4);
  EXPECT_EQ(t->colors.rim_focused, (Color{0xFF, 0x00, 0x00, 0x80}));
  // Untouched fields kept their base values.
  EXPECT_EQ(t->name, base.name);
  EXPECT_EQ(t->title_bar.height, base.title_bar.height);
  EXPECT_EQ(t->colors.shadow, base.colors.shadow);
  EXPECT_EQ(t->buttons.close.fill, base.buttons.close.fill);
}

TEST(CsdLoadThemeStringTest, MalformedTomlReturnsInvalidArgument) {
  auto t = drm::csd::load_theme_string("name = \"unterminated");
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(CsdLoadThemeStringTest, BadColorHexReturnsInvalidArgument) {
  auto t = drm::csd::load_theme_string(R"(
[colors]
panel_top = "not-a-color"
)");
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(CsdLoadThemeStringTest, WrongTypedColorReturnsInvalidArgument) {
  auto t = drm::csd::load_theme_string(R"(
[colors]
panel_top = 12345
)");
  ASSERT_FALSE(t.has_value());
}

// ── load_theme_file ─────────────────────────────────────────────

TEST(CsdLoadThemeFileTest, MissingFileReturnsEnoent) {
  auto t = drm::csd::load_theme_file("/nonexistent/theme.toml");
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error(), std::make_error_code(std::errc::no_such_file_or_directory));
}

// ── Built-in variants ───────────────────────────────────────────

TEST(CsdBuiltinThemesTest, GlassDefaultIsNamedAndPopulated) {
  const auto& t = drm::csd::glass_default_theme();
  EXPECT_EQ(t.name, "glass-default");
  EXPECT_GT(t.corner_radius, 0);
  EXPECT_GT(t.shadow_extent, 0);
  EXPECT_GT(t.animation_duration_ms, 0);
  EXPECT_FALSE(t.title_bar.font.empty());
  // Distinguishable colors for the three buttons (sanity).
  EXPECT_NE(t.buttons.close.fill, t.buttons.minimize.fill);
  EXPECT_NE(t.buttons.minimize.fill, t.buttons.maximize.fill);
}

TEST(CsdBuiltinThemesTest, LiteIsCheaperThanDefault) {
  const auto& def = drm::csd::glass_default_theme();
  const auto& lite = drm::csd::glass_lite_theme();
  EXPECT_EQ(lite.name, "glass-lite");
  EXPECT_LT(lite.shadow_extent, def.shadow_extent);
  EXPECT_LT(lite.animation_duration_ms, def.animation_duration_ms);
}

TEST(CsdBuiltinThemesTest, MinimalDropsShadowAndAnimation) {
  const auto& m = drm::csd::glass_minimal_theme();
  EXPECT_EQ(m.name, "glass-minimal");
  EXPECT_EQ(m.shadow_extent, 0);
  EXPECT_EQ(m.animation_duration_ms, 0);
  EXPECT_EQ(m.colors.shadow.a, 0);
}