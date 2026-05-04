// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::csd::Renderer. The renderer paints into a
// drm::BufferMapping; tests fabricate the mapping over a heap buffer
// (no live KMS device needed) and pixel-probe the result.

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/csd/renderer.hpp>
#include <drm-cxx/csd/shadow_cache.hpp>
#include <drm-cxx/csd/theme.hpp>
#include <drm-cxx/csd/window_state.hpp>

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

using drm::csd::HoverButton;
using drm::csd::Renderer;
using drm::csd::RendererConfig;
using drm::csd::ShadowCache;
using drm::csd::WindowState;

namespace {

// One canvas + the BufferMapping that views it. The mapping holds
// the raw pointer + a no-op unmapper; the canvas vector outlives any
// mapping returned from view().
struct Canvas {
  std::uint32_t width;
  std::uint32_t height;
  std::vector<std::uint8_t> pixels;

  Canvas(std::uint32_t w, std::uint32_t h)
      : width(w),
        height(h),
        pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0U) {}

  drm::BufferMapping view(drm::MapAccess access = drm::MapAccess::ReadWrite) {
    return {pixels.data(), pixels.size(), width * 4U, width, height, access, nullptr, nullptr};
  }

  // Read alpha at (x, y). Layout is PRGB32 (BGRA bytes on LE).
  [[nodiscard]] std::uint8_t alpha_at(std::uint32_t x, std::uint32_t y) const {
    const std::size_t off = ((static_cast<std::size_t>(y) * width) + x) * 4U;
    return pixels[off + 3U];
  }

  // Read packed 0xAARRGGBB at (x, y). Useful for "did *any* color
  // land here" checks rather than per-channel decomposition.
  [[nodiscard]] std::uint32_t packed_at(std::uint32_t x, std::uint32_t y) const {
    const std::size_t off = ((static_cast<std::size_t>(y) * width) + x) * 4U;
    return (static_cast<std::uint32_t>(pixels[off + 3U]) << 24U) |
           (static_cast<std::uint32_t>(pixels[off + 2U]) << 16U) |
           (static_cast<std::uint32_t>(pixels[off + 1U]) << 8U) |
           static_cast<std::uint32_t>(pixels[off + 0U]);
  }
};

}  // namespace

// ── Construction ────────────────────────────────────────────────

TEST(CsdRenderer, ConstructWithDefaultsDoesNotCrash) {
  const Renderer r;
  // has_font may be true or false depending on whether the host has
  // any of the well-known font paths available — both are valid.
  (void)r.has_font();
}

TEST(CsdRenderer, NoSystemFontMeansNoFont) {
  RendererConfig cfg;
  cfg.try_system_font = false;
  const Renderer r(std::move(cfg));
  EXPECT_FALSE(r.has_font());
}

TEST(CsdRenderer, MoveCtorWorks) {
  Renderer a;
  const Renderer b(std::move(a));
  (void)b.has_font();
}

// ── Error paths ────────────────────────────────────────────────

TEST(CsdRendererDraw, EmptyMappingFails) {
  Renderer r;
  ShadowCache cache;
  drm::BufferMapping empty;
  auto rc = r.draw(drm::csd::glass_default_theme(), WindowState{}, empty, cache);
  EXPECT_FALSE(rc.has_value());
}

// ── Output validation ──────────────────────────────────────────

TEST(CsdRendererDraw, ProducesNonZeroAlphaInsidePanel) {
  Renderer r;
  ShadowCache cache;
  Canvas c(128, 96);
  WindowState state;
  state.title = "test";
  state.focused = true;

  auto map = c.view();
  ASSERT_TRUE(r.draw(drm::csd::glass_default_theme(), state, map, cache).has_value());

  // Center of the canvas sits well inside the panel rect.
  EXPECT_GT(c.alpha_at(c.width / 2, c.height / 2), 0U);
}

TEST(CsdRendererDraw, CornerPixelsStayTransparentBeyondShadow) {
  // glass_minimal has shadow_extent = 0 — the panel fills the full
  // canvas, so the four absolute corners must clip via the rounded-
  // rect to alpha 0.
  Renderer r;
  ShadowCache cache;
  Canvas c(64, 64);
  auto map = c.view();
  ASSERT_TRUE(r.draw(drm::csd::glass_minimal_theme(), WindowState{}, map, cache).has_value());

  EXPECT_EQ(c.alpha_at(0, 0), 0U);
  EXPECT_EQ(c.alpha_at(c.width - 1, 0), 0U);
  EXPECT_EQ(c.alpha_at(0, c.height - 1), 0U);
  EXPECT_EQ(c.alpha_at(c.width - 1, c.height - 1), 0U);
}

TEST(CsdRendererDraw, IsDeterministic) {
  Renderer r;
  ShadowCache cache;
  Canvas a(96, 64);
  Canvas b(96, 64);

  WindowState state;
  state.focused = true;

  auto map_a = a.view();
  auto map_b = b.view();
  ASSERT_TRUE(r.draw(drm::csd::glass_default_theme(), state, map_a, cache).has_value());
  ASSERT_TRUE(r.draw(drm::csd::glass_default_theme(), state, map_b, cache).has_value());

  EXPECT_EQ(a.pixels, b.pixels);
}

TEST(CsdRendererDraw, FocusedAndBlurredDifferAtRim) {
  Renderer r;
  ShadowCache cache;
  Canvas focused(96, 64);
  Canvas blurred(96, 64);

  WindowState fs;
  fs.focused = true;
  WindowState bs;
  bs.focused = false;

  auto map_f = focused.view();
  auto map_b = blurred.view();
  ASSERT_TRUE(r.draw(drm::csd::glass_default_theme(), fs, map_f, cache).has_value());
  ASSERT_TRUE(r.draw(drm::csd::glass_default_theme(), bs, map_b, cache).has_value());

  // The rim is a 1-px stroke around the panel; pixels along it should
  // differ between focused and blurred. Sample a row near the top of
  // the panel (inside the shadow margin so it lands on the rim).
  const auto& theme = drm::csd::glass_default_theme();
  const auto rim_y = static_cast<std::uint32_t>(theme.shadow_extent);
  bool any_diff = false;
  for (std::uint32_t x = 0; x < focused.width; ++x) {
    if (focused.packed_at(x, rim_y) != blurred.packed_at(x, rim_y)) {
      any_diff = true;
      break;
    }
  }
  EXPECT_TRUE(any_diff);
}

TEST(CsdRendererDraw, HoverChangesPixelsInButtonRegion) {
  Renderer r;
  ShadowCache cache;
  Canvas none(160, 64);
  Canvas hover(160, 64);

  WindowState ns;
  ns.focused = true;
  ns.hover = HoverButton::None;
  WindowState hs;
  hs.focused = true;
  hs.hover = HoverButton::Close;

  auto map_n = none.view();
  auto map_h = hover.view();
  ASSERT_TRUE(r.draw(drm::csd::glass_default_theme(), ns, map_n, cache).has_value());
  ASSERT_TRUE(r.draw(drm::csd::glass_default_theme(), hs, map_h, cache).has_value());

  // Buttons sit on the right side of the title bar. Sweep the right
  // ~50 px and look for any difference.
  bool any_diff = false;
  const auto& theme = drm::csd::glass_default_theme();
  const std::uint32_t row = static_cast<std::uint32_t>(theme.shadow_extent) +
                            static_cast<std::uint32_t>(theme.title_bar.height / 2);
  for (std::uint32_t x = none.width - 50; x < none.width - 5; ++x) {
    if (none.packed_at(x, row) != hover.packed_at(x, row)) {
      any_diff = true;
      break;
    }
  }
  EXPECT_TRUE(any_diff);
}

TEST(CsdRendererDraw, ShadowCachePopulatedAfterDraw) {
  Renderer r;
  ShadowCache cache;
  Canvas c(128, 96);

  EXPECT_EQ(cache.size(), 0U);

  WindowState state;
  state.focused = true;
  auto map = c.view();
  ASSERT_TRUE(r.draw(drm::csd::glass_default_theme(), state, map, cache).has_value());

  // Default theme has shadow_extent > 0, so the renderer asked the
  // cache to blit a shadow patch — that blit must have inserted an
  // entry.
  EXPECT_EQ(cache.size(), 1U);
}
