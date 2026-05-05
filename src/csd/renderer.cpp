// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "renderer.hpp"

#include "../log.hpp"

#include <drm-cxx/detail/expected.hpp>

// Blend2D's header layout differs across distros (Fedora puts it under
// blend2d/, some older Debians at the top level). Mirror the same
// __has_include guard + NOLINT used in capture/snapshot.cpp; when
// neither path resolves, the TU degrades gracefully so clang-tidy
// passes that don't see -isystem still type-check the rest.
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#define DRM_CXX_CSD_HAS_BL2D
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#define DRM_CXX_CSD_HAS_BL2D
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace drm::csd {

namespace {

drm::unexpected<std::error_code> err(std::errc code) {
  return drm::unexpected<std::error_code>(std::make_error_code(code));
}

#ifdef DRM_CXX_CSD_HAS_BL2D

// NOLINTBEGIN(misc-include-cleaner) — same Blend2D umbrella caveat as
// capture/snapshot.cpp.

// Same well-known font path list the signage_player overlay uses.
// Trying these in order avoids pulling fontconfig as a hard dep.
constexpr std::array<const char*, 10> k_font_candidates = {
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/Vera.ttf",
};

bool load_font_face(const RendererConfig& cfg, BLFontFace& out) {
  if (!cfg.font_path.empty()) {
    if (out.create_from_file(cfg.font_path.c_str()) == BL_SUCCESS) {
      return true;
    }
    drm::log_warn("csd::Renderer: font_path '{}' failed to load; falling back", cfg.font_path);
  }
  if (!cfg.try_system_font) {
    return false;
  }
  for (const char* path : k_font_candidates) {
    if (out.create_from_file(path) == BL_SUCCESS) {
      return true;
    }
  }
  drm::log_warn(
      "csd::Renderer: no system font found in well-known paths; "
      "title text will be skipped");
  return false;
}

// 64×64 deterministic noise tile, generated once. A small LCG seeded
// by a fixed constant keeps the pattern reproducible across runs (and
// across processes — useful for golden-image diffs).
BLImage make_noise_tile() {
  constexpr int k_tile = 64;
  BLImage img;
  if (img.create(k_tile, k_tile, BL_FORMAT_PRGB32) != BL_SUCCESS) {
    return img;
  }
  BLImageData data{};
  if (img.make_mutable(&data) != BL_SUCCESS) {
    return img;
  }
  // LCG params from Numerical Recipes — small state, decent visual
  // entropy at this scale; we never use it for anything cryptographic.
  std::uint32_t s = 0xCAFEBABEU;
  auto next = [&s]() {
    s = (s * 1664525U) + 1013904223U;
    return s;
  };
  for (int y = 0; y < k_tile; ++y) {
    auto* row = static_cast<std::uint8_t*>(data.pixel_data) +
                (static_cast<std::ptrdiff_t>(y) * data.stride);
    for (int x = 0; x < k_tile; ++x) {
      const auto v = static_cast<std::uint8_t>(next() & 0xFFU);
      // PRGB32 layout: BGRA bytes, fully opaque grayscale tile. The
      // renderer multiplies this in via BL_COMP_OP_MULTIPLY at the
      // amplitude the theme requests.
      row[(x * 4) + 0] = v;
      row[(x * 4) + 1] = v;
      row[(x * 4) + 2] = v;
      row[(x * 4) + 3] = 0xFF;
    }
  }
  return img;
}

BLRgba32 to_bl(Color c) noexcept {
  return BLRgba32{c.packed_argb()};
}

float clamp01_or_default(float v, float fallback) noexcept {
  // Sentinel < 0 means "no animator wired": fall back to the binary
  // visual derived from state.focused / state.hover so v1 callers
  // keep their behavior. Real progress values come in clamped.
  if (v < 0.0F) {
    return fallback;
  }
  return std::clamp(v, 0.0F, 1.0F);
}

std::uint8_t lerp_u8(std::uint8_t a, std::uint8_t b, float t) noexcept {
  const auto fa = static_cast<float>(a);
  const auto fb = static_cast<float>(b);
  return static_cast<std::uint8_t>(std::clamp(fa + ((fb - fa) * t), 0.0F, 255.0F));
}

BLRgba32 lerp_color(Color a, Color b, float t) noexcept {
  return BLRgba32{
      Color{lerp_u8(a.r, b.r, t), lerp_u8(a.g, b.g, t), lerp_u8(a.b, b.b, t), lerp_u8(a.a, b.a, t)}
          .packed_argb()};
}

void draw_button(BLContext& ctx, double cx, double cy, double radius, Color fill_color,
                 Color hover_color, float hover_weight) {
  // Radial gradient: a touch lighter at the top-left, theme color at
  // the bottom-right. Gives the button a faux-spherical highlight.
  // The middle stop lerps fill→hover by hover_weight so a fading
  // button reads as the in-between tint rather than a binary swap.
  const BLRgba32 fill = to_bl(fill_color);
  const BLRgba32 top = lerp_color(fill_color, hover_color, hover_weight);
  BLGradient g(BLRadialGradientValues(cx - 1.5, cy - 1.5, cx, cy, radius));
  g.add_stop(0.0, BLRgba32(0xFFFFFFFFU));
  g.add_stop(0.4, top);
  g.add_stop(1.0, fill);
  ctx.fill_circle(BLCircle(cx, cy, radius), g);
  // Thin outer ring for definition against light backgrounds.
  ctx.stroke_circle(BLCircle(cx, cy, radius - 0.5), BLRgba32(0x33000000U));
}

void draw_glass(BLImage& target, const Theme& theme, const WindowState& state, ShadowCache& shadows,
                BLFontFace& font_face, BLImage& noise_tile, bool has_font) {
  const int w = target.width();
  const int h = target.height();
  if (w <= 0 || h <= 0) {
    return;
  }
  const auto geom =
      decoration_geometry(theme, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
  const int extent = std::max(0, theme.shadow_extent);
  const int panel_x = geom.panel_x;
  const int panel_y = geom.panel_y;
  const int panel_w = geom.panel_w;
  const int panel_h = geom.panel_h;
  const int radius = std::min({theme.corner_radius, panel_w / 2, panel_h / 2});

  BLContext ctx(target);
  ctx.clear_all();

  // Continuous focus weight in [0, 1]. Drives the shadow cross-fade,
  // the rim color lerp, and (indirectly) frames where the renderer
  // would otherwise have flipped binary values mid-fade.
  const float fp = clamp01_or_default(state.focus_progress, state.focused ? 1.0F : 0.0F);

  // ── Step 2: shadow patch ─────────────────────────────────
  if (panel_w > 0 && panel_h > 0 && extent > 0) {
    BLImageData td{};
    if (target.make_mutable(&td) == BL_SUCCESS) {
      ShadowDest dst;
      dst.pixels = static_cast<std::uint8_t*>(td.pixel_data);
      dst.stride = static_cast<std::uint32_t>(td.stride);
      dst.width = static_cast<std::uint32_t>(w);
      dst.height = static_cast<std::uint32_t>(h);
      const std::uint64_t tid = theme_id(theme);
      const ShadowKey k_blurred{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                Elevation::Blurred, tid};
      const ShadowKey k_focused{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                Elevation::Focused, tid};
      shadows.blit_cross_fade(k_blurred, k_focused, theme, dst, fp);
    }
  }

  // From here forward we paint with Blend2D over whatever the cache
  // wrote. SRC_OVER is the default for the BLContext, so each step
  // composites correctly on top of the soft shadow halo.
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  if (panel_w <= 0 || panel_h <= 0) {
    ctx.end();
    return;
  }

  // ── Step 3: panel gradient ───────────────────────────────
  {
    BLGradient g(BLLinearGradientValues(0, panel_y, 0, panel_y + panel_h));
    g.add_stop(0.0, to_bl(theme.colors.panel_top));
    g.add_stop(1.0, to_bl(theme.colors.panel_bottom));
    ctx.fill_round_rect(BLRoundRect(panel_x, panel_y, panel_w, panel_h, radius), g);
  }

  // ── Step 4: specular highlight ───────────────────────────
  {
    ctx.save();
    BLPath clip;
    clip.add_round_rect(BLRoundRect(panel_x, panel_y, panel_w, panel_h, radius));
    ctx.clip_to_rect(BLRect(panel_x, panel_y, panel_w, 4.0));
    ctx.set_comp_op(BL_COMP_OP_SCREEN);
    BLGradient g(BLLinearGradientValues(0, panel_y, 0, panel_y + 4));
    g.add_stop(0.0, BLRgba32(0xCCFFFFFFU));
    g.add_stop(1.0, BLRgba32(0x00FFFFFFU));
    ctx.fill_path(clip, g);
    ctx.restore();
  }

  // ── Step 5: frosted noise ────────────────────────────────
  if (theme.noise_amplitude > 0.0 && noise_tile.width() > 0) {
    ctx.save();
    BLPath clip;
    clip.add_round_rect(BLRoundRect(panel_x, panel_y, panel_w, panel_h, radius));
    ctx.clip_to_rect(BLRect(panel_x, panel_y, panel_w, panel_h));
    ctx.set_comp_op(BL_COMP_OP_MULTIPLY);
    // Amplitude scales the global alpha so weak noise at 0.04 reads
    // as a subtle grain rather than a 50%-gray multiply.
    const double amp = std::clamp(theme.noise_amplitude, 0.0, 1.0);
    ctx.set_global_alpha(amp);
    BLPattern pat(noise_tile);
    pat.set_extend_mode(BL_EXTEND_MODE_REPEAT);
    ctx.fill_path(clip, pat);
    ctx.set_global_alpha(1.0);
    ctx.restore();
  }

  // ── Step 6: title text ───────────────────────────────────
  if (has_font && !state.title.empty() && theme.title_bar.font_size > 0) {
    BLFont font;
    if (font.create_from_face(font_face, static_cast<float>(theme.title_bar.font_size)) ==
        BL_SUCCESS) {
      // Vertically center within the title bar; horizontally inset
      // from the left edge.
      const double tx = panel_x + 12.0;
      const double ty =
          panel_y + (theme.title_bar.height * 0.5) + (theme.title_bar.font_size * 0.35);
      // 2-pass shadow: dark below, then the actual text on top. Keeps
      // light titles legible against the translucent panel.
      ctx.fill_utf8_text(BLPoint(tx + 1.0, ty + 1.0), font, state.title.data(), state.title.size(),
                         to_bl(theme.colors.title_shadow));
      ctx.fill_utf8_text(BLPoint(tx, ty), font, state.title.data(), state.title.size(),
                         to_bl(theme.colors.title_text));
    }
  }

  // ── Step 7: buttons (close, minimize, maximize) ──────────
  if (theme.title_bar.height > 0) {
    // Layout — including the Linux-conventional Close-rightmost order
    // — comes from decoration_geometry so the shell's hit-test reads
    // the same numbers we paint with.
    const auto cy = static_cast<double>(geom.button_cy);
    const auto button_r = static_cast<double>(geom.button_radius);
    struct ButtonSpec {
      HoverButton which;
      const Theme::Buttons::Button* style;
      int cx;
    };
    const std::array<ButtonSpec, 3> specs = {{
        {HoverButton::Close, &theme.buttons.close, geom.close_cx},
        {HoverButton::Minimize, &theme.buttons.minimize, geom.minimize_cx},
        {HoverButton::Maximize, &theme.buttons.maximize, geom.maximize_cx},
    }};
    for (const auto& s : specs) {
      const float hp =
          state.hover == s.which ? clamp01_or_default(state.hover_progress, 1.0F) : 0.0F;
      draw_button(ctx, static_cast<double>(s.cx), cy, button_r, s.style->fill, s.style->hover, hp);
    }
  }

  // ── Step 8: rim ──────────────────────────────────────────
  {
    const BLRgba32 rim = lerp_color(theme.colors.rim_blurred, theme.colors.rim_focused, fp);
    ctx.set_stroke_width(1.0);
    // Inner stroke: inset by 0.5 px so the rim sits on the panel
    // pixels rather than straddling them.
    ctx.stroke_round_rect(
        BLRoundRect(panel_x + 0.5, panel_y + 0.5, panel_w - 1.0, panel_h - 1.0, radius), rim);
  }

  ctx.end();
}

// NOLINTEND(misc-include-cleaner)

#endif  // DRM_CXX_CSD_HAS_BL2D

}  // namespace

DecorationGeometry decoration_geometry(const Theme& theme, std::uint32_t deco_w,
                                       std::uint32_t deco_h) noexcept {
  // Visible button radius and the gutters around it. Tuned to match
  // the macOS / GNOME traffic-light scale; if these change the
  // renderer paints them and the shell hit-tests them at the same
  // values because both go through this struct.
  constexpr int k_button_radius = 7;
  constexpr int k_button_gap = 6;
  constexpr int k_button_right_pad = 10;

  DecorationGeometry g;
  const int extent = std::max(0, theme.shadow_extent);
  g.panel_x = extent;
  g.panel_y = extent;
  g.panel_w = std::max(0, static_cast<int>(deco_w) - (2 * extent));
  g.panel_h = std::max(0, static_cast<int>(deco_h) - (2 * extent));
  g.title_bar_height = std::max(0, theme.title_bar.height);

  g.button_radius = k_button_radius;
  g.button_cy = g.panel_y + (g.title_bar_height / 2);
  const int right_edge = g.panel_x + g.panel_w - k_button_right_pad;
  g.close_cx = right_edge - k_button_radius;
  const int step = (2 * k_button_radius) + k_button_gap;
  g.minimize_cx = g.close_cx - step;
  g.maximize_cx = g.minimize_cx - step;
  return g;
}

#ifdef DRM_CXX_CSD_HAS_BL2D

// NOLINTBEGIN(misc-include-cleaner)

struct Renderer::Impl {
  RendererConfig cfg;
  BLFontFace font_face;
  BLImage noise_tile;
  bool has_font{false};
};

Renderer::Renderer(RendererConfig cfg) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = std::move(cfg);
  impl_->has_font = load_font_face(impl_->cfg, impl_->font_face);
  impl_->noise_tile = make_noise_tile();
}

Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

bool Renderer::has_font() const noexcept {
  return impl_ != nullptr && impl_->has_font;
}

drm::expected<void, std::error_code> Renderer::draw(const Theme& theme, const WindowState& state,
                                                    drm::BufferMapping& target,
                                                    ShadowCache& shadows) {
  if (impl_ == nullptr) {
    return err(std::errc::invalid_argument);
  }
  if (target.empty() || target.width() == 0 || target.height() == 0) {
    return err(std::errc::invalid_argument);
  }
  // Wrap the mapping as BL_FORMAT_PRGB32 (matches DRM_FORMAT_ARGB8888
  // scanout) with RW access so the BLContext can read the shadow
  // pixels the cache wrote and SRC_OVER on top of them.
  BLImage img;
  const BLResult wr = img.create_from_data(
      static_cast<int>(target.width()), static_cast<int>(target.height()), BL_FORMAT_PRGB32,
      target.pixels().data(), static_cast<intptr_t>(target.stride()), BL_DATA_ACCESS_RW, nullptr,
      nullptr);
  if (wr != BL_SUCCESS) {
    return err(std::errc::io_error);
  }
  draw_glass(img, theme, state, shadows, impl_->font_face, impl_->noise_tile, impl_->has_font);
  return {};
}

// NOLINTEND(misc-include-cleaner)

#else  // !DRM_CXX_CSD_HAS_BL2D

// Degraded TU — present so the symbol exists for the link, but every
// path returns an error. Real builds always have Blend2D available
// when DRM_CXX_HAS_BLEND2D is on; this branch only fires under tidy
// runs that compile the TU without -isystem flags.
struct Renderer::Impl {
  RendererConfig cfg;
  bool has_font{false};
};
Renderer::Renderer(RendererConfig cfg) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = std::move(cfg);
}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
bool Renderer::has_font() const noexcept {
  return false;
}
drm::expected<void, std::error_code> Renderer::draw(const Theme&, const WindowState&,
                                                    drm::BufferMapping&, ShadowCache&) {
  return err(std::errc::function_not_supported);
}

#endif  // DRM_CXX_CSD_HAS_BL2D

}  // namespace drm::csd
