// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cluster_sim — automotive instrument-cluster showcase. Currently:
// a Blend2D-painted radial-gradient backdrop plus animated speedometer
// and tachometer dial layers, a digital speed readout between them,
// and a row of warning indicators below. An optional V4l2DecoderSource
// rear-view layer lands in a follow-up.
//
// Layer stack (bottom up):
//   * Bg: full-screen XRGB8888 dumb buffer, painted once at startup
//     (radial gradient on a dark-blue backdrop).
//   * Speedometer dial: ARGB8888 dumb buffer, repainted each frame
//     with an animated needle. Positioned in the screen's left third.
//   * Tachometer dial: same shape, screen's right third, slightly
//     out-of-phase animation so the two dials don't sweep in lockstep.
//   * Center info: ARGB8888 readout between the dials. Big speed
//     number derived from the speedometer's animation phase, "km/h"
//     label below.
//   * Warning indicators: ARGB8888 strip below the dials. Four icons
//     (left turn, check engine, high beam, right turn) each blinking
//     on independent periods so the demo shows them in different lit
//     states. zpos >= 4 so the allocator pins them on a hardware
//     plane even when the dials had to composite -- mirrors how a
//     real cluster never lets a warning indicator drop below 60 fps.
//
// The dials, center info, and warnings all demonstrate per-frame
// Blend2D paint into a dumb buffer the LayerScene scans out of
// directly. There's no double-buffering -- the buffer is mapped,
// painted, unmapped, and committed each frame; brief tearing on the
// needle is acceptable for an idle demo and would be addressed by
// alternating two DumbBufferSources per layer if a real cluster
// needed tear-free animation.
//
// Key bindings:
//   Esc / q / Ctrl-C — quit.
//   Ctrl+Alt+F<n>    — VT switch (forwarded to libseat).

#include "../../common/open_output.hpp"
#include "../../common/vt_switch.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

// Blend2D ships its umbrella header at <blend2d/blend2d.h> on the
// upstream source install + Fedora; older Debian/Ubuntu packages put
// it at <blend2d.h>. Cover both via __has_include, mirroring the
// project's existing pattern in capture/snapshot.cpp.
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#else
#error "Blend2D header not found despite the build's blend2d gate being on"
#endif

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <variant>

namespace {

// Cluster-aesthetic palette. Center is near-black with a subtle blue
// cast (matches the "deep instrument" look most automotive clusters
// settle on); edges fade darker so the dial layers have somewhere to
// read against.
constexpr std::uint32_t k_bg_center_argb = 0xFF101626U;
constexpr std::uint32_t k_bg_edge_argb = 0xFF02040AU;

// Dial palette. Face is darker than the bg center so the dial reads
// as separate from the backdrop; the rim is a neutral metallic gray.
constexpr std::uint32_t k_dial_rim_argb = 0xFF606870U;
constexpr std::uint32_t k_dial_face_argb = 0xFF080C18U;
constexpr std::uint32_t k_dial_tick_argb = 0xFFC0C8D0U;
constexpr std::uint32_t k_dial_hub_argb = 0xFF1A1F2CU;
constexpr std::uint32_t k_speedo_needle_argb = 0xFFFF3B30U;  // red
constexpr std::uint32_t k_tach_needle_argb = 0xFFFFB300U;    // amber

// Idle animation periods (seconds). Out of phase so the two dials
// don't sweep in lockstep -- a real cluster's dials are decorrelated
// (speed and RPM track different physical signals).
constexpr double k_speedo_period_s = 6.0;
constexpr double k_tach_period_s = 4.0;

// Conventional automotive cluster sweep range: 270° starting from
// the bottom-left and going clockwise to the bottom-right. In
// screen-with-Y-down coordinates (which match Blend2D), the bottom-
// left direction is 3π/4 and a clockwise sweep is positive.
constexpr double k_pi = 3.14159265358979323846;
constexpr double k_dial_start_angle = 3.0 * k_pi / 4.0;
constexpr double k_dial_sweep_angle = 3.0 * k_pi / 2.0;
constexpr int k_dial_major_ticks = 12;

// Center info layer dimensions (between the dials). Width covers
// "240 km/h" comfortably; height fits a big number plus the unit
// label below.
constexpr std::uint32_t k_info_w = 320U;
constexpr std::uint32_t k_info_h = 140U;
constexpr std::uint32_t k_info_speed_max_kmh = 240U;
constexpr std::uint32_t k_info_text_argb = 0xFFE6E8EEU;
constexpr std::uint32_t k_info_unit_argb = 0xFF7C8090U;
constexpr float k_info_speed_font_px = 96.0F;
constexpr float k_info_unit_font_px = 22.0F;

// Warning indicator strip. Four cells, each ~80 px wide; total width
// 4*100 = 400 with internal padding.
constexpr std::uint32_t k_warn_w = 400U;
constexpr std::uint32_t k_warn_h = 80U;
constexpr int k_warn_count = 4;
constexpr float k_warn_glyph_font_px = 36.0F;

// Each indicator has its own color for the lit state and its own
// blink period. Decorrelated periods so the strip looks organic
// rather than metronomic.
struct WarningSpec {
  std::uint32_t lit_argb;
  double period_s;
  std::string_view glyph;
};
constexpr std::array<WarningSpec, k_warn_count> k_warn_specs{{
    {0xFF34C759U, 0.55, "<"},  // left turn — green arrow stand-in
    {0xFFFFB300U, 0.83, "!"},  // check engine — amber bang
    {0xFF4DA8FFU, 1.20, "^"},  // high beam — blue chevron
    {0xFF34C759U, 0.62, ">"},  // right turn — green arrow stand-in
}};
constexpr std::uint32_t k_warn_dim_argb = 0xFF14181FU;  // unlit cell fill
constexpr std::uint32_t k_warn_glyph_dim_argb = 0xFF38404CU;
constexpr std::uint32_t k_warn_cell_radius = 12U;

// Convert an idle-animation phase in [0, 1] to a needle-position
// norm in [0, 1] via a (1 - cos)/2 sweep. Smooth at the endpoints,
// peaks in the middle, mirrors back -- the cosine sweep gives a
// slow-fast-slow visual that reads more "dial easing" than a
// triangle wave.
[[nodiscard]] double dial_norm_from_phase(double phase01) noexcept {
  return 0.5 * (1.0 - std::cos(2.0 * k_pi * phase01));
}

// Paint a radial gradient into an XRGB8888 dumb buffer. Blend2D
// initializes a BLImage view over the existing pixel buffer (no copy)
// and the BLContext flushes on `end()` before we return.
//
// NOLINTBEGIN(misc-include-cleaner) — Blend2D types reach this TU
// through the <blend2d/blend2d.h> umbrella; include-cleaner can't
// resolve the per-symbol header given the umbrella's macro guards.
// Same suppression as signage_player/overlay_renderer.cpp.
void paint_bg_gradient(drm::BufferMapping& mapping, std::uint32_t width,
                       std::uint32_t height) noexcept {
  if (width == 0U || height == 0U) {
    return;
  }
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(height) * stride) {
    return;
  }

  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(width), static_cast<int>(height), BL_FORMAT_XRGB32,
                              pixels.data(), static_cast<intptr_t>(stride), BL_DATA_ACCESS_RW,
                              nullptr, nullptr) != BL_SUCCESS) {
    return;
  }

  BLContext ctx(canvas);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);

  double const cx = static_cast<double>(width) / 2.0;
  double const cy = static_cast<double>(height) / 2.0;
  double const radius = static_cast<double>(width > height ? width : height) * 0.6;
  BLGradient grad(BLRadialGradientValues(cx, cy, cx, cy, radius));
  grad.add_stop(0.0, BLRgba32(k_bg_center_argb));
  grad.add_stop(1.0, BLRgba32(k_bg_edge_argb));
  ctx.fill_all(grad);
  ctx.end();
}

// Paint a dial face + animated needle into an ARGB8888 dumb buffer.
// `norm` in [0, 1] maps onto the dial's 270° sweep range; `needle_argb`
// distinguishes speedo (red) from tach (amber). Painting clears the
// buffer to transparent first so the dial's circular face renders
// against the bg layer beneath.
void paint_dial(drm::BufferMapping& mapping, std::uint32_t size, double norm,
                std::uint32_t needle_argb) noexcept {
  if (size == 0U) {
    return;
  }
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(size) * stride) {
    return;
  }

  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(size), static_cast<int>(size), BL_FORMAT_PRGB32,
                              pixels.data(), static_cast<intptr_t>(stride), BL_DATA_ACCESS_RW,
                              nullptr, nullptr) != BL_SUCCESS) {
    return;
  }

  BLContext ctx(canvas);
  // Clear to transparent first so the dial reads as a circular cut-out
  // against the bg layer underneath. Subsequent paint ops use SRC_OVER
  // for proper alpha blending of the rim, face, ticks, and needle.
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0x00000000U));
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  double const cx = static_cast<double>(size) / 2.0;
  double const cy = static_cast<double>(size) / 2.0;
  double const r_outer = static_cast<double>(size) * 0.48;
  double const r_inner = static_cast<double>(size) * 0.42;
  double const r_tick_outer = static_cast<double>(size) * 0.46;
  double const r_tick_inner = static_cast<double>(size) * 0.38;
  double const r_needle = static_cast<double>(size) * 0.40;
  double const r_hub = static_cast<double>(size) * 0.06;

  // Rim (filled circle in metallic gray) and face (slightly inset
  // dark fill) — fill_circle layers correctly because of SRC_OVER.
  ctx.fill_circle(BLCircle(cx, cy, r_outer), BLRgba32(k_dial_rim_argb));
  ctx.fill_circle(BLCircle(cx, cy, r_inner), BLRgba32(k_dial_face_argb));

  // Major ticks (12 + 1 to close the sweep range) — light gray lines
  // running radially outward across the rim. Using set_stroke_width
  // ahead of stroke_line because Blend2D's per-call stroke API doesn't
  // take a width inline.
  double const a_norm = std::clamp(norm, 0.0, 1.0);
  ctx.set_stroke_width(3.0);
  for (int i = 0; i <= k_dial_major_ticks; ++i) {
    double const t = static_cast<double>(i) / static_cast<double>(k_dial_major_ticks);
    double const a = k_dial_start_angle + (t * k_dial_sweep_angle);
    double const x1 = cx + (r_tick_inner * std::cos(a));
    double const y1 = cy + (r_tick_inner * std::sin(a));
    double const x2 = cx + (r_tick_outer * std::cos(a));
    double const y2 = cy + (r_tick_outer * std::sin(a));
    ctx.stroke_line(BLPoint(x1, y1), BLPoint(x2, y2), BLRgba32(k_dial_tick_argb));
  }

  // Needle: thick rounded line from center to the tip computed from
  // the normalized position. Stroke cap is round so the tip reads as
  // a smooth point rather than a chopped rectangle.
  double const needle_a = k_dial_start_angle + (a_norm * k_dial_sweep_angle);
  double const nx = cx + (r_needle * std::cos(needle_a));
  double const ny = cy + (r_needle * std::sin(needle_a));
  ctx.set_stroke_width(5.0);
  ctx.set_stroke_caps(BL_STROKE_CAP_ROUND);
  ctx.stroke_line(BLPoint(cx, cy), BLPoint(nx, ny), BLRgba32(needle_argb));

  // Hub cap covering the needle's pivot point.
  ctx.fill_circle(BLCircle(cx, cy, r_hub), BLRgba32(k_dial_hub_argb));

  ctx.end();
}

// Try a handful of well-known Linux font paths in priority order.
// fc-match would be the principled answer but pulls fontconfig as a
// hard dep for a scaffold-tier example; mirror signage_player's
// approach and probe the most common Fedora / Debian / Arch / Alpine
// layouts. Returning BL_ERROR_FONT_NOT_INITIALIZED is non-fatal
// upstream -- the center-info / warning paint paths fall back to
// shape-only output when the font face is empty.
[[nodiscard]] BLResult load_default_font_face(BLFontFace& face) noexcept {
  static constexpr std::array<const char*, 10> k_candidates = {
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf",
      "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/TTF/Vera.ttf",
  };
  for (const char* path : k_candidates) {
    if (face.create_from_file(path) == BL_SUCCESS) {
      return BL_SUCCESS;
    }
  }
  return BL_ERROR_FONT_NOT_INITIALIZED;
}

// Paint a centered UTF-8 string at the requested baseline-y. Helper
// shared by the center-info readout and the warning glyph cells.
// Caller has already prepared the BLContext + the font face; this
// just handles glyph shaping + centering math.
void paint_centered_text(BLContext& ctx, BLFont& font, std::string_view text, double cx,
                         double cy_baseline, std::uint32_t argb) noexcept {
  if (text.empty()) {
    return;
  }
  BLGlyphBuffer gb;
  gb.set_utf8_text(text.data(), text.size());
  if (font.shape(gb) != BL_SUCCESS) {
    return;
  }
  BLTextMetrics tm{};
  if (font.get_text_metrics(gb, tm) != BL_SUCCESS) {
    return;
  }
  double const text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
  double const x = cx - (text_w * 0.5) - tm.bounding_box.x0;
  ctx.fill_utf8_text(BLPoint(x, cy_baseline), font, text.data(), text.size(), BLRgba32(argb));
}

// Paint the speed readout: one big numeric value (0-240) with a
// "km/h" label beneath. `font_face` may be empty when the host had no
// usable system font -- in that case the layer is left transparent
// rather than emitting a placeholder, since a wrong-looking text
// fallback reads worse than a missing one.
void paint_center_info(drm::BufferMapping& mapping, std::uint32_t speed_kmh,
                       const BLFontFace& font_face) noexcept {
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(k_info_h) * stride) {
    return;
  }
  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(k_info_w), static_cast<int>(k_info_h),
                              BL_FORMAT_PRGB32, pixels.data(), static_cast<intptr_t>(stride),
                              BL_DATA_ACCESS_RW, nullptr, nullptr) != BL_SUCCESS) {
    return;
  }
  BLContext ctx(canvas);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0x00000000U));
  if (!font_face.is_valid()) {
    ctx.end();
    return;
  }
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  // Speed digits centered horizontally at ~70% height; "km/h" label
  // sits directly below at ~95% height. Baseline-y values picked by
  // eye to match the typical instrument-cluster layout.
  std::string const speed_text = std::to_string(std::clamp<std::uint32_t>(speed_kmh, 0, 999));
  BLFont speed_font;
  if (speed_font.create_from_face(font_face, k_info_speed_font_px) == BL_SUCCESS) {
    paint_centered_text(ctx, speed_font, speed_text, static_cast<double>(k_info_w) * 0.5,
                        static_cast<double>(k_info_h) * 0.72, k_info_text_argb);
  }
  BLFont unit_font;
  if (unit_font.create_from_face(font_face, k_info_unit_font_px) == BL_SUCCESS) {
    paint_centered_text(ctx, unit_font, "km/h", static_cast<double>(k_info_w) * 0.5,
                        static_cast<double>(k_info_h) * 0.95, k_info_unit_argb);
  }
  ctx.end();
}

// Paint the four-cell warning strip. `phases[i]` in [0, 1) is the
// per-cell blink phase; lit when phase < 0.5, dim otherwise. Each
// cell is a rounded rectangle filled with the lit/dim color and a
// single glyph centered inside.
void paint_warning_indicators(drm::BufferMapping& mapping,
                              const std::array<double, k_warn_count>& phases,
                              const BLFontFace& font_face) noexcept {
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(k_warn_h) * stride) {
    return;
  }
  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(k_warn_w), static_cast<int>(k_warn_h),
                              BL_FORMAT_PRGB32, pixels.data(), static_cast<intptr_t>(stride),
                              BL_DATA_ACCESS_RW, nullptr, nullptr) != BL_SUCCESS) {
    return;
  }

  BLContext ctx(canvas);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0x00000000U));
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  BLFont glyph_font;
  bool const have_font = font_face.is_valid() &&
                         glyph_font.create_from_face(font_face, k_warn_glyph_font_px) == BL_SUCCESS;

  double const cell_w = static_cast<double>(k_warn_w) / static_cast<double>(k_warn_count);
  double const inner_pad = 6.0;
  for (int i = 0; i < k_warn_count; ++i) {
    bool const lit = phases.at(static_cast<std::size_t>(i)) < 0.5;
    auto const& spec = k_warn_specs.at(static_cast<std::size_t>(i));
    std::uint32_t const cell_argb = lit ? spec.lit_argb : k_warn_dim_argb;
    std::uint32_t const glyph_argb = lit ? 0xFF080C18U : k_warn_glyph_dim_argb;

    double const x0 = (static_cast<double>(i) * cell_w) + inner_pad;
    double const y0 = inner_pad;
    double const w = cell_w - (2.0 * inner_pad);
    double const h = static_cast<double>(k_warn_h) - (2.0 * inner_pad);
    ctx.fill_round_rect(BLRoundRect(x0, y0, w, h, k_warn_cell_radius), BLRgba32(cell_argb));

    if (have_font) {
      double const cx = x0 + (w * 0.5);
      double const cy_baseline = y0 + (h * 0.7);
      paint_centered_text(ctx, glyph_font, spec.glyph, cx, cy_baseline, glyph_argb);
    }
  }
  ctx.end();
}
// NOLINTEND(misc-include-cleaner)

}  // namespace

int main(int argc, char** argv) {
  auto ctx_opt = drm::examples::open_and_pick_output(argc, argv);
  if (!ctx_opt.has_value()) {
    return EXIT_FAILURE;
  }
  auto& ctx = *ctx_opt;
  auto& dev = ctx.device;
  auto& seat = ctx.seat;
  std::uint32_t const fb_w = ctx.mode.hdisplay;
  std::uint32_t const fb_h = ctx.mode.vdisplay;

  // Bg layer — painted once, scanned out forever (until step 2 starts
  // mutating per-frame state on the dial layers above it).
  auto bg_src_r = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src_r) {
    drm::println(stderr, "DumbBufferSource::create (bg): {}", bg_src_r.error().message());
    return EXIT_FAILURE;
  }
  auto bg_src = std::move(*bg_src_r);
  auto* bg_src_raw = bg_src.get();
  if (auto m = bg_src->map(drm::MapAccess::Write); m) {
    paint_bg_gradient(*m, fb_w, fb_h);
  } else {
    drm::println(stderr, "bg paint: map failed: {}", m.error().message());
    return EXIT_FAILURE;
  }

  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = ctx.crtc_id;
  scene_cfg.connector_id = ctx.connector_id;
  scene_cfg.mode = ctx.mode;
  auto scene_r = drm::scene::LayerScene::create(dev, scene_cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(bg_src);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  if (auto h = scene->add_layer(std::move(bg_desc)); !h) {
    drm::println(stderr, "add_layer (bg): {}", h.error().message());
    return EXIT_FAILURE;
  }

  // Dial sizing: 4/9 of screen height capped at 400 px gives ~400 on
  // 1080p, 320 on 720p, scales down sensibly for smaller outputs.
  // Centered vertically; speedo at 1/4-screen, tach at 3/4-screen.
  std::uint32_t const dial_size = std::min<std::uint32_t>(fb_h * 4U / 9U, 400U);
  auto const dial_y = static_cast<std::int32_t>((fb_h - dial_size) / 2U);
  auto const speedo_x =
      static_cast<std::int32_t>(fb_w / 4U) - static_cast<std::int32_t>(dial_size / 2U);
  auto const tach_x =
      static_cast<std::int32_t>((fb_w * 3U) / 4U) - static_cast<std::int32_t>(dial_size / 2U);

  auto make_dial_layer =
      [&](std::int32_t x,
          std::int32_t y) -> drm::expected<drm::scene::DumbBufferSource*, std::error_code> {
    auto src_r =
        drm::scene::DumbBufferSource::create(dev, dial_size, dial_size, DRM_FORMAT_ARGB8888);
    if (!src_r) {
      return drm::unexpected<std::error_code>(src_r.error());
    }
    auto src = std::move(*src_r);
    auto* raw = src.get();
    drm::scene::LayerDesc desc;
    desc.source = std::move(src);
    desc.display.src_rect = drm::scene::Rect{0, 0, dial_size, dial_size};
    desc.display.dst_rect = drm::scene::Rect{x, y, dial_size, dial_size};
    // amdgpu pins PRIMARY at zpos=2; the dials need to sit above the
    // bg, so anchor them at zpos>=3 explicitly to avoid silent
    // collision with the primary plane.
    desc.display.zpos = 3;
    if (auto h = scene->add_layer(std::move(desc)); !h) {
      return drm::unexpected<std::error_code>(h.error());
    }
    return raw;
  };

  auto speedo_r = make_dial_layer(speedo_x, dial_y);
  if (!speedo_r) {
    drm::println(stderr, "add_layer (speedo): {}", speedo_r.error().message());
    return EXIT_FAILURE;
  }
  auto tach_r = make_dial_layer(tach_x, dial_y);
  if (!tach_r) {
    drm::println(stderr, "add_layer (tach): {}", tach_r.error().message());
    return EXIT_FAILURE;
  }
  auto* speedo_src_raw = *speedo_r;
  auto* tach_src_raw = *tach_r;

  // Center info layer: ARGB8888 between the dials, vertically
  // centered against the dial midline. Uses the same zpos as the
  // dials -- it's a sibling UI layer, not a priority overlay.
  auto info_src_r =
      drm::scene::DumbBufferSource::create(dev, k_info_w, k_info_h, DRM_FORMAT_ARGB8888);
  if (!info_src_r) {
    drm::println(stderr, "DumbBufferSource::create (info): {}", info_src_r.error().message());
    return EXIT_FAILURE;
  }
  auto* info_src_raw = info_src_r->get();
  drm::scene::LayerDesc info_desc;
  info_desc.source = std::move(*info_src_r);
  info_desc.display.src_rect = drm::scene::Rect{0, 0, k_info_w, k_info_h};
  auto const info_x =
      static_cast<std::int32_t>(fb_w / 2U) - static_cast<std::int32_t>(k_info_w / 2U);
  auto const info_y =
      dial_y + static_cast<std::int32_t>(dial_size / 2U) - static_cast<std::int32_t>(k_info_h / 2U);
  info_desc.display.dst_rect = drm::scene::Rect{info_x, info_y, k_info_w, k_info_h};
  info_desc.display.zpos = 3;
  if (auto h = scene->add_layer(std::move(info_desc)); !h) {
    drm::println(stderr, "add_layer (info): {}", h.error().message());
    return EXIT_FAILURE;
  }

  // Warning indicators: 4-cell ARGB strip below the dials. zpos=4
  // (above the dials) so the allocator pins it on a hardware plane
  // even when the dials had to composite -- a real cluster never
  // lets a warning indicator drop frames.
  auto warn_src_r =
      drm::scene::DumbBufferSource::create(dev, k_warn_w, k_warn_h, DRM_FORMAT_ARGB8888);
  if (!warn_src_r) {
    drm::println(stderr, "DumbBufferSource::create (warn): {}", warn_src_r.error().message());
    return EXIT_FAILURE;
  }
  auto* warn_src_raw = warn_src_r->get();
  drm::scene::LayerDesc warn_desc;
  warn_desc.source = std::move(*warn_src_r);
  warn_desc.display.src_rect = drm::scene::Rect{0, 0, k_warn_w, k_warn_h};
  auto const warn_x =
      static_cast<std::int32_t>(fb_w / 2U) - static_cast<std::int32_t>(k_warn_w / 2U);
  auto const warn_y = dial_y + static_cast<std::int32_t>(dial_size) + 40;
  warn_desc.display.dst_rect = drm::scene::Rect{warn_x, warn_y, k_warn_w, k_warn_h};
  warn_desc.display.zpos = 4;
  if (auto h = scene->add_layer(std::move(warn_desc)); !h) {
    drm::println(stderr, "add_layer (warn): {}", h.error().message());
    return EXIT_FAILURE;
  }

  // Best-effort font load. If no usable font is installed, the info
  // + warning paints fall back to shape-only output (transparent
  // info, glyph-less warning cells). The dials don't need a font.
  BLFontFace font_face;
  if (load_default_font_face(font_face) != BL_SUCCESS) {
    drm::println(stderr,
                 "cluster_sim: no usable system font found -- center info + warning glyphs "
                 "will render shape-only");
  }

  // Paint each animated layer at zero/initial state so the first
  // commit has valid pixel content everywhere. repaint_layers is the
  // shared per-frame paint path called from the main loop and from
  // the session-resume cleanup.
  auto repaint_layers = [&](double speedo_norm, double tach_norm, std::uint32_t speed_kmh,
                            const std::array<double, k_warn_count>& warn_phases) {
    if (auto m = speedo_src_raw->map(drm::MapAccess::Write); m) {
      paint_dial(*m, dial_size, speedo_norm, k_speedo_needle_argb);
    }
    if (auto m = tach_src_raw->map(drm::MapAccess::Write); m) {
      paint_dial(*m, dial_size, tach_norm, k_tach_needle_argb);
    }
    if (auto m = info_src_raw->map(drm::MapAccess::Write); m) {
      paint_center_info(*m, speed_kmh, font_face);
    }
    if (auto m = warn_src_raw->map(drm::MapAccess::Write); m) {
      paint_warning_indicators(*m, warn_phases, font_face);
    }
  };
  std::array<double, k_warn_count> const initial_warn_phases{0.0, 0.0, 0.0, 0.0};
  repaint_layers(0.0, 0.0, 0U, initial_warn_phases);

  bool flip_pending = false;
  // need_repaint drives the per-frame dial-paint cycle. Set true on
  // every page-flip-event landing so the next loop iteration repaints
  // and commits, giving us a flip-driven ~vsync-locked animation
  // cadence without a wall-clock timer.
  bool need_repaint = false;
  drm::PageFlip page_flip(dev);
  page_flip.set_handler([&](std::uint32_t /*c*/, std::uint64_t /*s*/, std::uint64_t /*t*/) {
    flip_pending = false;
    need_repaint = true;
  });

  // libinput keyboard + VT-switch chord forwarding. libseat puts the
  // TTY in KD_GRAPHICS where the kernel suppresses Ctrl-C signal
  // generation, so the libinput keyboard is the only reliable in-app
  // quit path on a real VT.
  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_r = drm::input::Seat::open({}, std::move(libinput_opener));
  if (!input_seat_r) {
    drm::println(stderr, "drm::input::Seat::open: {} (need root or 'input' group membership)",
                 input_seat_r.error().message());
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_r;
  bool quit = false;
  drm::examples::VtChordTracker vt_chord;
  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    const auto* ke = std::get_if<drm::input::KeyboardEvent>(&event);
    if (ke == nullptr) {
      return;
    }
    if (vt_chord.observe(*ke, seat ? &*seat : nullptr)) {
      return;
    }
    if (vt_chord.is_quit_key(*ke)) {
      quit = true;
    }
  });

  // Session pause/resume bookkeeping. Both flags are touched from the
  // main thread only — libseat callbacks fire from inside
  // seat->dispatch(), which runs in the main loop. Defer the actual
  // device-fd swap until the loop's next iteration so we don't tear
  // down PageFlip mid-dispatch.
  bool session_paused = false;
  int pending_resume_fd = -1;
  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      flip_pending = false;
      scene->on_session_paused();
      (void)input_seat.suspend();
    });
    seat->set_resume_callback([&](std::string_view path, int new_fd) {
      if (path.substr(0, 9) != "/dev/dri/") {
        return;
      }
      pending_resume_fd = new_fd;
      session_paused = false;
      (void)input_seat.resume();
    });
  }

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit failed: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flip_pending = true;
  auto const start_time = std::chrono::steady_clock::now();
  drm::println("cluster_sim: {}x{} — Esc / Q to quit", fb_w, fb_h);

  pollfd pfds[3]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;

  while (!quit) {
    int const timeout = flip_pending ? 16 : -1;
    if (int const ret = poll(pfds, 3, timeout); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      (void)input_seat.dispatch();
    }
    if ((pfds[1].revents & POLLIN) != 0) {
      (void)page_flip.dispatch(0);
    }
    if ((pfds[2].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }

    if (pending_resume_fd != -1) {
      int const new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      ctx.device = drm::Device::from_fd(new_fd);
      pfds[1].fd = dev.fd();
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed");
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed");
        break;
      }
      if (auto r = scene->on_session_resumed(dev); !r) {
        drm::println(stderr, "resume: on_session_resumed: {}", r.error().message());
        break;
      }
      page_flip = drm::PageFlip(dev);
      page_flip.set_handler([&](std::uint32_t, std::uint64_t, std::uint64_t) {
        flip_pending = false;
        need_repaint = true;
      });
      // Buffer mappings were torn down on pause; repaint the bg + both
      // dials against the fresh mappings before the resume commit so
      // the post-resume frame is intact rather than stale-or-blank.
      if (auto m = bg_src_raw->map(drm::MapAccess::Write); m) {
        paint_bg_gradient(*m, fb_w, fb_h);
      }
      double const elapsed_resume =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
      double const speedo_resume = dial_norm_from_phase(elapsed_resume / k_speedo_period_s);
      double const tach_resume = dial_norm_from_phase(elapsed_resume / k_tach_period_s);
      auto const speed_resume_kmh =
          static_cast<std::uint32_t>(speedo_resume * static_cast<double>(k_info_speed_max_kmh));
      std::array<double, k_warn_count> warn_phases_resume{};
      for (int i = 0; i < k_warn_count; ++i) {
        double const period = k_warn_specs.at(static_cast<std::size_t>(i)).period_s;
        warn_phases_resume.at(static_cast<std::size_t>(i)) =
            std::fmod(elapsed_resume / period, 1.0);
      }
      repaint_layers(speedo_resume, tach_resume, speed_resume_kmh, warn_phases_resume);
      if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
        drm::println(stderr, "resume commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
    }

    // Per-frame paint cycle: only when the prior flip has landed and
    // the session is live. Repaint both dials against their current
    // animation phase, then commit -- the kernel queues the flip for
    // the next vblank, the page-flip handler clears flip_pending and
    // sets need_repaint again, and the loop ticks at scanout cadence.
    if (need_repaint && !flip_pending && !session_paused) {
      need_repaint = false;
      double const elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
      double const speedo_norm = dial_norm_from_phase(elapsed / k_speedo_period_s);
      double const tach_norm = dial_norm_from_phase(elapsed / k_tach_period_s);
      auto const speed_kmh =
          static_cast<std::uint32_t>(speedo_norm * static_cast<double>(k_info_speed_max_kmh));
      std::array<double, k_warn_count> warn_phases{};
      for (int i = 0; i < k_warn_count; ++i) {
        double const period = k_warn_specs.at(static_cast<std::size_t>(i)).period_s;
        warn_phases.at(static_cast<std::size_t>(i)) = std::fmod(elapsed / period, 1.0);
      }
      repaint_layers(speedo_norm, tach_norm, speed_kmh, warn_phases);
      auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
      if (!r) {
        if (r.error() == std::errc::permission_denied) {
          // Master got revoked between the flip event and the next
          // commit (libseat hasn't fired pause_cb yet). Treat as a
          // soft pause and let the resume path put us back together.
          session_paused = true;
          flip_pending = false;
          continue;
        }
        drm::println(stderr, "commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
    }
  }

  return EXIT_SUCCESS;
}
