// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "signage_player/playlist.hpp"

#include <gtest/gtest.h>
#include <string_view>

namespace {

constexpr std::string_view kMinimal = R"(
[[slide]]
kind = "color"
color = "#ff8800"
duration_ms = 1500
)";

constexpr std::string_view kFull = R"(
[[slide]]
kind = "color"
color = "#102030"

[[slide]]
kind = "png"
source = "/tmp/banner.png"
duration_ms = 5000

[[slide]]
kind = "blend2d"
source = "demo:gradient"

[[slide]]
kind = "thorvg"
source = "/tmp/anim.tvg"

[overlay]
kind = "text"
text = "Hello"
font_size = 48
fg_color = "#ffffff"
bg_color = "#80112233"

[ticker]
text = "BREAKING NEWS"
font_size = 28
fg_color = "#ffeeaa"
bg_color = "#c0102030"
pixels_per_second = 200

[clock]
format = "%H:%M:%S"
font_size = 64
fg_color = "#ffeeaa"
bg_color = "#80112233"

[logo]
path = "/tmp/brand.png"
width = 128
height = 64
fallback_color = "#ff112233"
)";

}  // namespace

TEST(SignagePlaylist, ParseMinimal) {
  auto p = signage::Playlist::parse(kMinimal);
  ASSERT_TRUE(p.has_value()) << p.error().message();
  ASSERT_EQ(p->slides().size(), 1U);
  EXPECT_EQ(p->slides()[0].kind, signage::SlideKind::Color);
  EXPECT_EQ(p->slides()[0].color, 0xFFFF8800U);
  EXPECT_EQ(p->slides()[0].duration_ms, 1500U);
  EXPECT_FALSE(p->overlay().has_value());
}

TEST(SignagePlaylist, ParseFull) {
  auto p = signage::Playlist::parse(kFull);
  ASSERT_TRUE(p.has_value()) << p.error().message();
  ASSERT_EQ(p->slides().size(), 4U);
  EXPECT_EQ(p->slides()[0].kind, signage::SlideKind::Color);
  EXPECT_EQ(p->slides()[0].color, 0xFF102030U);
  EXPECT_EQ(p->slides()[1].kind, signage::SlideKind::Png);
  EXPECT_EQ(p->slides()[1].source, "/tmp/banner.png");
  EXPECT_EQ(p->slides()[1].duration_ms, 5000U);
  EXPECT_EQ(p->slides()[2].kind, signage::SlideKind::Blend2D);
  EXPECT_EQ(p->slides()[3].kind, signage::SlideKind::Thorvg);
  ASSERT_TRUE(p->overlay().has_value());
  EXPECT_EQ(p->overlay()->text, "Hello");
  EXPECT_EQ(p->overlay()->font_size, 48U);
  EXPECT_EQ(p->overlay()->fg_color, 0xFFFFFFFFU);
  // "#80112233" reads as RR=80, GG=11, BB=22, AA=33 → packed AARRGGBB.
  EXPECT_EQ(p->overlay()->bg_color, 0x33801122U);
  ASSERT_TRUE(p->ticker().has_value());
  EXPECT_EQ(p->ticker()->text, "BREAKING NEWS");
  EXPECT_EQ(p->ticker()->font_size, 28U);
  EXPECT_EQ(p->ticker()->fg_color, 0xFFFFEEAAU);
  // "#c0102030" reads as RR=c0, GG=10, BB=20, AA=30 → packed AARRGGBB.
  EXPECT_EQ(p->ticker()->bg_color, 0x30C01020U);
  EXPECT_EQ(p->ticker()->pixels_per_second, 200U);
  ASSERT_TRUE(p->clock().has_value());
  EXPECT_EQ(p->clock()->format, "%H:%M:%S");
  EXPECT_EQ(p->clock()->font_size, 64U);
  EXPECT_EQ(p->clock()->fg_color, 0xFFFFEEAAU);
  // "#80112233" reads as RR=80, GG=11, BB=22, AA=33 → packed AARRGGBB.
  EXPECT_EQ(p->clock()->bg_color, 0x33801122U);
  ASSERT_TRUE(p->logo().has_value());
  EXPECT_EQ(p->logo()->path, "/tmp/brand.png");
  EXPECT_EQ(p->logo()->width, 128U);
  EXPECT_EQ(p->logo()->height, 64U);
  // "#ff112233" reads as RR=ff, GG=11, BB=22, AA=33 → packed AARRGGBB.
  EXPECT_EQ(p->logo()->fallback_color, 0x33FF1122U);
}

TEST(SignagePlaylist, ClockOmittedByDefault) {
  auto p = signage::Playlist::parse(kMinimal);
  ASSERT_TRUE(p.has_value()) << p.error().message();
  EXPECT_FALSE(p->clock().has_value());
}

TEST(SignagePlaylist, ClockDefaultsApplied) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color"
color = "#ffffff"

[clock])");
  ASSERT_TRUE(p.has_value()) << p.error().message();
  ASSERT_TRUE(p->clock().has_value());
  EXPECT_EQ(p->clock()->format, "%H:%M");
  EXPECT_EQ(p->clock()->font_size, 48U);
  EXPECT_EQ(p->clock()->fg_color, 0xFFFFFFFFU);
  EXPECT_EQ(p->clock()->bg_color, 0x80000000U);
}

TEST(SignagePlaylist, ClockRejectsEmptyFormat) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color"
color = "#ffffff"

[clock]
format = "")");
  EXPECT_FALSE(p.has_value());
}

TEST(SignagePlaylist, TickerOmittedByDefault) {
  auto p = signage::Playlist::parse(kMinimal);
  ASSERT_TRUE(p.has_value()) << p.error().message();
  EXPECT_FALSE(p->ticker().has_value());
}

TEST(SignagePlaylist, TickerWithoutTextRejected) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color"
color = "#ffffff"

[ticker]
font_size = 24)");
  EXPECT_FALSE(p.has_value());
}

TEST(SignagePlaylist, TickerDefaultsApplied) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color"
color = "#ffffff"

[ticker]
text = "headlines")");
  ASSERT_TRUE(p.has_value()) << p.error().message();
  ASSERT_TRUE(p->ticker().has_value());
  EXPECT_EQ(p->ticker()->text, "headlines");
  EXPECT_EQ(p->ticker()->font_size, 24U);
  EXPECT_EQ(p->ticker()->fg_color, 0xFFFFFFFFU);
  EXPECT_EQ(p->ticker()->bg_color, 0xC0000000U);
  EXPECT_EQ(p->ticker()->pixels_per_second, 120U);
}

TEST(SignagePlaylist, LogoOmittedByDefault) {
  auto p = signage::Playlist::parse(kMinimal);
  ASSERT_TRUE(p.has_value()) << p.error().message();
  EXPECT_FALSE(p->logo().has_value());
}

TEST(SignagePlaylist, LogoDefaultsApplied) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color"
color = "#ffffff"

[logo]
path = "/tmp/x.png")");
  ASSERT_TRUE(p.has_value()) << p.error().message();
  ASSERT_TRUE(p->logo().has_value());
  EXPECT_EQ(p->logo()->path, "/tmp/x.png");
  EXPECT_EQ(p->logo()->width, 96U);
  EXPECT_EQ(p->logo()->height, 96U);
  EXPECT_EQ(p->logo()->fallback_color, 0x00000000U);
}

TEST(SignagePlaylist, LogoWithoutPathRejected) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color"
color = "#ffffff"

[logo]
width = 128)");
  EXPECT_FALSE(p.has_value());
}

TEST(SignagePlaylist, RejectsEmpty) {
  auto p = signage::Playlist::parse("");
  EXPECT_FALSE(p.has_value());
}

TEST(SignagePlaylist, RejectsNoSlides) {
  auto p = signage::Playlist::parse("[overlay]\nkind=\"text\"\ntext=\"hi\"\n");
  EXPECT_FALSE(p.has_value());
}

TEST(SignagePlaylist, RejectsUnknownKind) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "nope"
color = "#ffffff")");
  EXPECT_FALSE(p.has_value());
}

TEST(SignagePlaylist, RejectsBadColor) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color"
color = "ff8800")");
  EXPECT_FALSE(p.has_value());
}

TEST(SignagePlaylist, RejectsColorWithoutColor) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color")");
  EXPECT_FALSE(p.has_value());
}

TEST(SignagePlaylist, RejectsPngWithoutSource) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "png")");
  EXPECT_FALSE(p.has_value());
}

TEST(SignagePlaylist, ParsesSixDigitColorAsOpaque) {
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color"
color = "#abcdef")");
  ASSERT_TRUE(p.has_value()) << p.error().message();
  EXPECT_EQ(p->slides()[0].color, 0xFFABCDEFU);
}

TEST(SignagePlaylist, ParsesEightDigitColorWithAlpha) {
  // RR=80, GG=ab, BB=cd, AA=ef → packed AARRGGBB = 0xef80abcd.
  auto p = signage::Playlist::parse(R"([[slide]]
kind = "color"
color = "#80abcdef")");
  ASSERT_TRUE(p.has_value()) << p.error().message();
  EXPECT_EQ(p->slides()[0].color, 0xEF80ABCDU);
}
