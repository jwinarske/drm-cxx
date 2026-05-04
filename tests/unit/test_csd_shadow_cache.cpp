// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::csd::ShadowCache + theme_id(). Pure CPU tests:
// no live device, no Blend2D context — the cache is intentionally
// Blend2D-free so the renderer is the only consumer that drags BL in.

#include <drm-cxx/csd/shadow_cache.hpp>
#include <drm-cxx/csd/theme.hpp>

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using drm::csd::Elevation;
using drm::csd::ShadowCache;
using drm::csd::ShadowDest;
using drm::csd::ShadowKey;
using drm::csd::Theme;
using drm::csd::theme_id;

namespace {

// Allocate a zero-filled PRGB32 destination buffer for blit_into.
std::vector<std::uint8_t> make_dest(std::uint32_t w, std::uint32_t h) {
  return std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U, 0U);
}

ShadowDest dest_view(std::vector<std::uint8_t>& buf, std::uint32_t w, std::uint32_t h) {
  ShadowDest d;
  d.pixels = buf.data();
  d.stride = w * 4U;
  d.width = w;
  d.height = h;
  return d;
}

// Sum every alpha byte in a PRGB32 buffer. Useful as a coarse "did
// the shadow leave any visible pixels?" probe.
std::uint64_t alpha_sum(const std::vector<std::uint8_t>& buf) {
  std::uint64_t s = 0;
  for (std::size_t i = 3; i < buf.size(); i += 4U) {
    s += buf[i];
  }
  return s;
}

}  // namespace

// ── theme_id ────────────────────────────────────────────────────

TEST(CsdShadowCacheThemeId, StableAcrossRebuilds) {
  EXPECT_EQ(theme_id(drm::csd::glass_default_theme()), theme_id(drm::csd::glass_default_theme()));
}

TEST(CsdShadowCacheThemeId, DiffersOnColorChange) {
  Theme a = drm::csd::glass_default_theme();
  Theme b = a;
  b.colors.shadow.r ^= 0x01U;
  EXPECT_NE(theme_id(a), theme_id(b));
}

TEST(CsdShadowCacheThemeId, IgnoresName) {
  Theme a = drm::csd::glass_default_theme();
  Theme b = a;
  b.name = "renamed";
  EXPECT_EQ(theme_id(a), theme_id(b));
}

TEST(CsdShadowCacheThemeId, IgnoresAnimationDuration) {
  // animation_duration_ms doesn't affect the rasterized shadow.
  Theme a = drm::csd::glass_default_theme();
  Theme b = a;
  b.animation_duration_ms += 100;
  EXPECT_EQ(theme_id(a), theme_id(b));
}

TEST(CsdShadowCacheThemeId, DiffersOnShadowExtentChange) {
  Theme a = drm::csd::glass_default_theme();
  Theme b = a;
  b.shadow_extent += 1;
  EXPECT_NE(theme_id(a), theme_id(b));
}

// ── ShadowCache lifecycle ───────────────────────────────────────

TEST(CsdShadowCache, DefaultCapacityIsEightWhenZeroRequested) {
  ShadowCache c(0);
  EXPECT_EQ(c.capacity(), ShadowCache::kDefaultCapacity);
  EXPECT_EQ(c.size(), 0U);
}

TEST(CsdShadowCache, RespectsExplicitCapacity) {
  ShadowCache c(3);
  EXPECT_EQ(c.capacity(), 3U);
}

TEST(CsdShadowCache, MoveCtorTransfersState) {
  ShadowCache a(4);
  ShadowCache b(std::move(a));
  EXPECT_EQ(b.capacity(), 4U);
}

TEST(CsdShadowCache, ClearEmptiesIndex) {
  ShadowCache c(4);
  const Theme& t = drm::csd::glass_default_theme();
  ShadowKey k{64, 64, Elevation::Focused, theme_id(t)};
  auto buf = make_dest(64, 64);
  ASSERT_TRUE(c.blit_into(k, t, dest_view(buf, 64, 64)));
  EXPECT_EQ(c.size(), 1U);
  c.clear();
  EXPECT_EQ(c.size(), 0U);
  EXPECT_FALSE(c.contains(k));
}

// ── blit_into ───────────────────────────────────────────────────

TEST(CsdShadowCacheBlit, RejectsZeroSizedKey) {
  ShadowCache c;
  const Theme& t = drm::csd::glass_default_theme();
  auto buf = make_dest(16, 16);
  EXPECT_FALSE(c.blit_into(ShadowKey{0, 16, Elevation::Focused, theme_id(t)}, t,
                           dest_view(buf, 16, 16)));
  EXPECT_FALSE(c.blit_into(ShadowKey{16, 0, Elevation::Focused, theme_id(t)}, t,
                           dest_view(buf, 16, 16)));
}

TEST(CsdShadowCacheBlit, RejectsNullDest) {
  ShadowCache c;
  const Theme& t = drm::csd::glass_default_theme();
  ShadowDest d{};
  EXPECT_FALSE(c.blit_into(ShadowKey{16, 16, Elevation::Focused, theme_id(t)}, t, d));
}

TEST(CsdShadowCacheBlit, FirstCallPopulatesAlpha) {
  ShadowCache c;
  const Theme& t = drm::csd::glass_default_theme();
  ShadowKey k{96, 96, Elevation::Focused, theme_id(t)};
  auto buf = make_dest(96, 96);
  ASSERT_TRUE(c.blit_into(k, t, dest_view(buf, 96, 96)));
  EXPECT_GT(alpha_sum(buf), 0U);  // soft halo wrote some alpha into the buffer
  EXPECT_EQ(c.size(), 1U);
  EXPECT_TRUE(c.contains(k));
}

TEST(CsdShadowCacheBlit, SecondCallIsCacheHit) {
  ShadowCache c;
  const Theme& t = drm::csd::glass_default_theme();
  ShadowKey k{96, 96, Elevation::Focused, theme_id(t)};

  auto buf1 = make_dest(96, 96);
  ASSERT_TRUE(c.blit_into(k, t, dest_view(buf1, 96, 96)));
  EXPECT_EQ(c.size(), 1U);

  auto buf2 = make_dest(96, 96);
  ASSERT_TRUE(c.blit_into(k, t, dest_view(buf2, 96, 96)));
  EXPECT_EQ(c.size(), 1U);  // still one entry — cache hit, no insert
  EXPECT_EQ(buf1, buf2);    // identical pixels
}

TEST(CsdShadowCacheBlit, FocusedAndBlurredAreDistinctEntries) {
  ShadowCache c;
  const Theme& t = drm::csd::glass_default_theme();
  ShadowKey kf{96, 96, Elevation::Focused, theme_id(t)};
  ShadowKey kb{96, 96, Elevation::Blurred, theme_id(t)};

  auto buf_f = make_dest(96, 96);
  auto buf_b = make_dest(96, 96);
  ASSERT_TRUE(c.blit_into(kf, t, dest_view(buf_f, 96, 96)));
  ASSERT_TRUE(c.blit_into(kb, t, dest_view(buf_b, 96, 96)));

  EXPECT_EQ(c.size(), 2U);
  // Blurred elevation paints at 70% intensity → strictly less alpha
  // sum than the focused entry.
  EXPECT_GT(alpha_sum(buf_f), alpha_sum(buf_b));
}

TEST(CsdShadowCacheBlit, EvictsOldestPastCapacity) {
  ShadowCache c(2);
  const Theme& t = drm::csd::glass_default_theme();
  const auto tid = theme_id(t);

  ShadowKey ka{64, 64, Elevation::Focused, tid};
  ShadowKey kb{72, 72, Elevation::Focused, tid};
  ShadowKey kc{80, 80, Elevation::Focused, tid};

  auto buf = make_dest(96, 96);
  ASSERT_TRUE(c.blit_into(ka, t, dest_view(buf, 64, 64)));
  ASSERT_TRUE(c.blit_into(kb, t, dest_view(buf, 72, 72)));
  EXPECT_EQ(c.size(), 2U);
  EXPECT_TRUE(c.contains(ka));

  // Inserting kc evicts ka (oldest) — kb was just used so it stays.
  ASSERT_TRUE(c.blit_into(kc, t, dest_view(buf, 80, 80)));
  EXPECT_EQ(c.size(), 2U);
  EXPECT_FALSE(c.contains(ka));
  EXPECT_TRUE(c.contains(kb));
  EXPECT_TRUE(c.contains(kc));
}

TEST(CsdShadowCacheBlit, LruRefreshKeepsRecentlyUsedEntry) {
  ShadowCache c(2);
  const Theme& t = drm::csd::glass_default_theme();
  const auto tid = theme_id(t);

  ShadowKey ka{64, 64, Elevation::Focused, tid};
  ShadowKey kb{72, 72, Elevation::Focused, tid};
  ShadowKey kc{80, 80, Elevation::Focused, tid};

  auto buf = make_dest(96, 96);
  ASSERT_TRUE(c.blit_into(ka, t, dest_view(buf, 64, 64)));
  ASSERT_TRUE(c.blit_into(kb, t, dest_view(buf, 72, 72)));
  // Touch ka — moves it back to the front.
  ASSERT_TRUE(c.blit_into(ka, t, dest_view(buf, 64, 64)));
  // kc evicts kb (now the oldest).
  ASSERT_TRUE(c.blit_into(kc, t, dest_view(buf, 80, 80)));

  EXPECT_TRUE(c.contains(ka));
  EXPECT_FALSE(c.contains(kb));
  EXPECT_TRUE(c.contains(kc));
}

TEST(CsdShadowCacheBlit, ZeroExtentProducesTransparent) {
  ShadowCache c;
  const Theme& t = drm::csd::glass_minimal_theme();  // shadow_extent == 0
  ShadowKey k{64, 64, Elevation::Focused, theme_id(t)};
  auto buf = make_dest(64, 64);
  ASSERT_TRUE(c.blit_into(k, t, dest_view(buf, 64, 64)));
  // Shadow color in glass-minimal is transparent (alpha 0); the blur
  // collapses to a no-op and tinting by alpha=0 gives zero pixels.
  EXPECT_EQ(alpha_sum(buf), 0U);
  EXPECT_EQ(c.size(), 1U);
}

TEST(CsdShadowCacheBlit, SmallerDestClipsRatherThanCrashes) {
  ShadowCache c;
  const Theme& t = drm::csd::glass_default_theme();
  ShadowKey k{96, 96, Elevation::Focused, theme_id(t)};
  // 32×32 destination but a 96×96 cached patch — should clip cleanly.
  auto buf = make_dest(32, 32);
  ASSERT_TRUE(c.blit_into(k, t, dest_view(buf, 32, 32)));
  // Buffer size unchanged, no out-of-bounds writes.
  EXPECT_EQ(buf.size(), 32U * 32U * 4U);
}
