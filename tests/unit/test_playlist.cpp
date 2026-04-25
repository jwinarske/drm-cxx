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
